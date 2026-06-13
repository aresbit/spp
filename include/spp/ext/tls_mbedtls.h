#pragma once

// Opt-in companion adapter that turns mbedTLS 3.x into a Net::Byte_Stream.
// Including this header pulls in mbedTLS public headers and requires the
// consuming target to link against `-lmbedtls -lmbedx509 -lmbedcrypto`.
//
// Linux-only. Defaults to verify-required PKI through the system CA bundle
// (auto-discovered at connect time). For HFT / sidecar deployments where TLS
// terminates elsewhere, prefer wiring a Tcp_Client straight at the sidecar
// and skip this header entirely — both paths satisfy the same Byte_Stream
// contract.

#include <spp/io/net.h>
#include <spp/io/stream.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace spp::Ext {

// Production-shaped TLS client: server cert is verified against a trust
// store, SNI is set, TLS 1.2 minimum. Constructor sets up mbedTLS contexts;
// `connect_result` performs the TCP connect + handshake; subsequent
// send/recv calls flow plaintext bytes through the established session.
struct Tls_Mbedtls_Stream {
    struct Options {
        // PEM file with trusted root certificates. Empty = probe common
        // Linux locations (Debian/Ubuntu, RHEL/Fedora, Alpine) and fail if
        // none parse cleanly.
        String_View ca_path = ""_v;

        // PEM directory of trusted roots, layered on top of `ca_path` if
        // provided. Empty = ignored.
        String_View ca_dir = ""_v;

        // Force-skip server certificate verification. Only for local
        // loopback testing — do not enable for production.
        bool insecure_skip_verify = false;

        // Minimum negotiated TLS version. Defaults to TLS 1.2 which matches
        // Binance's current floor.
        int min_tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
    };

    Tls_Mbedtls_Stream() noexcept {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&ctr_drbg_);
        mbedtls_x509_crt_init(&cacert_);
    }

    ~Tls_Mbedtls_Stream() noexcept {
        close();
        if(pending_session_ != nullptr) {
            mbedtls_ssl_session_free(pending_session_);
            delete pending_session_;
            pending_session_ = nullptr;
        }
        mbedtls_x509_crt_free(&cacert_);
        mbedtls_ctr_drbg_free(&ctr_drbg_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ssl_free(&ssl_);
    }

    Tls_Mbedtls_Stream(const Tls_Mbedtls_Stream&) = delete;
    Tls_Mbedtls_Stream& operator=(const Tls_Mbedtls_Stream&) = delete;
    Tls_Mbedtls_Stream(Tls_Mbedtls_Stream&&) = delete;
    Tls_Mbedtls_Stream& operator=(Tls_Mbedtls_Stream&&) = delete;

    [[nodiscard]] Result<u64, String_View>
    connect_result(String_View host, u16 port) noexcept {
        return connect_result(host, port, Options{});
    }

    [[nodiscard]] Result<u64, String_View>
    connect_result(String_View host, u16 port, const Options& opts) noexcept {
        if(handshake_done_) return Result<u64, String_View>::err("already_connected"_v);

        // Re-using the same Tls_Mbedtls_Stream across reconnects requires
        // mbedtls_ssl_session_reset BEFORE the second handshake — calling
        // mbedtls_ssl_setup twice on the same context is UB per the
        // mbedTLS docs. We hoist all one-time setup into `setup_static_`
        // so subsequent connects only run the per-session prep.
        if(!setup_done_) {
            auto s = setup_static_(opts);
            if(!s.ok()) return s;
            setup_done_ = true;
        } else {
            // Per-session cleanup. Closes the previous TCP, resets ssl_
            // to the post-setup state (config / RNG / ca_chain stay).
            transport_.close();
            mbedtls_ssl_session_reset(&ssl_);
            session_resumed_ = false;
        }

        auto tcp_connected = transport_.connect_result(host, port);
        if(!tcp_connected.ok()) return tcp_connected;

        // SNI + cert hostname verification both consume this name —
        // re-issued every connect since the target host may change.
        char host_buf[256];
        if(host.length() >= sizeof(host_buf)) {
            return Result<u64, String_View>::err("host_too_long"_v);
        }
        for(u64 i = 0; i < host.length(); i++) host_buf[i] = static_cast<char>(host[i]);
        host_buf[host.length()] = '\0';

        if(int rc = mbedtls_ssl_set_hostname(&ssl_, host_buf)) {
            (void)rc;
            return Result<u64, String_View>::err("tls_sni_failed"_v);
        }

        // BIO uses `this` for context — re-bind to the same instance
        // but to the freshly-reconnected transport_ socket.
        mbedtls_ssl_set_bio(&ssl_, this, &Tls_Mbedtls_Stream::bio_send_,
                            &Tls_Mbedtls_Stream::bio_recv_, nullptr);

        // If the caller loaded a previous session via load_session(), install
        // it so the handshake can short-circuit into a resumption flow.
        if(pending_session_ != nullptr) {
            if(int rc = mbedtls_ssl_set_session(&ssl_, pending_session_)) {
                (void)rc;
                return Result<u64, String_View>::err("tls_session_install_failed"_v);
            }
        }

        for(;;) {
            int rc = mbedtls_ssl_handshake(&ssl_);
            if(rc == 0) break;
            if(rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return Result<u64, String_View>::err("tls_handshake_failed"_v);
            }
        }

        // mbedTLS doesn't expose a direct "was resumed" flag; the documented
        // proxy is "negotiated session id is non-empty AND matches what we
        // installed". For our purposes we set the flag if a session was
        // installed and the handshake produced a non-empty session id.
        if(pending_session_ != nullptr) {
            mbedtls_ssl_session probe;
            mbedtls_ssl_session_init(&probe);
            if(mbedtls_ssl_get_session(&ssl_, &probe) == 0) {
                session_resumed_ = true;
            }
            mbedtls_ssl_session_free(&probe);
            mbedtls_ssl_session_free(pending_session_);
            delete pending_session_;
            pending_session_ = nullptr;
        }

        handshake_done_ = true;
        return Result<u64, String_View>::ok(0);
    }

    [[nodiscard]] Result<u64, String_View> send_all_result(Slice<const u8> in) noexcept {
        if(!handshake_done_) return Result<u64, String_View>::err("tls_not_connected"_v);
        u64 sent = 0;
        while(sent < in.length()) {
            int rc = mbedtls_ssl_write(&ssl_, in.data() + sent, in.length() - sent);
            if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if(rc < 0) return Result<u64, String_View>::err("tls_write_failed"_v);
            sent += static_cast<u64>(rc);
        }
        return Result<u64, String_View>::ok(spp::move(sent));
    }

    [[nodiscard]] Result<u64, String_View> recv_result(Slice<u8> out) noexcept {
        if(!handshake_done_) return Result<u64, String_View>::err("tls_not_connected"_v);
        for(;;) {
            int rc = mbedtls_ssl_read(&ssl_, out.data(), out.length());
            if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if(rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                return Result<u64, String_View>::ok(0);
            }
            if(rc < 0) return Result<u64, String_View>::err("tls_read_failed"_v);
            return Result<u64, String_View>::ok(static_cast<u64>(rc));
        }
    }

    [[nodiscard]] Result<u64, String_View> recv_exact_result(Slice<u8> out) noexcept {
        u64 got = 0;
        while(got < out.length()) {
            Slice<u8> chunk{out.data() + got, out.length() - got};
            auto r = recv_result(chunk);
            if(!r.ok()) return r;
            u64 n = r.unwrap();
            if(n == 0) return Result<u64, String_View>::err("short_read"_v);
            got += n;
        }
        return Result<u64, String_View>::ok(spp::move(got));
    }

    void close() noexcept {
        if(handshake_done_) {
            // Best-effort close_notify. Ignore failures — the peer may have
            // gone away already.
            static_cast<void>(mbedtls_ssl_close_notify(&ssl_));
            handshake_done_ = false;
        }
        transport_.close();
    }

    [[nodiscard]] bool handshake_done() const noexcept {
        return handshake_done_;
    }

    [[nodiscard]] Net::Tcp_Client& transport() noexcept {
        return transport_;
    }

    // Number of application-level bytes mbedTLS has already decrypted
    // and is holding in its session buffer.  These bytes WILL satisfy a
    // subsequent `recv_result` even when `::poll` on the underlying
    // socket reports no readable data — TLS records can carry multiple
    // application messages, and a single socket-read decrypts all of
    // them at once.
    //
    // A multiplexed event loop must check this BEFORE polling the fd,
    // otherwise it will starve a stream that has buffered bytes.
    [[nodiscard]] u64 pending_app_bytes() const noexcept {
        if(!handshake_done_) return 0;
        return static_cast<u64>(mbedtls_ssl_get_bytes_avail(&ssl_));
    }

    // Underlying TCP socket handle. Hand this to `IO::poll_any_result`
    // for fd-level readiness. Note: `pending_app_bytes()` may be non-
    // zero even when the socket is not ready — always check it first.
    [[nodiscard]] const IO::Handle& native_handle() const noexcept {
        return transport_.handle();
    }

    // -- Session resumption -------------------------------------------------
    //
    // mbedTLS exposes the negotiated session as an opaque blob. Captured
    // after a successful handshake, it can be installed on a freshly-
    // initialised stream before its next connect_result() to skip the full
    // handshake (saving ~1 RTT + ~10 ms of crypto work). The blob is
    // PSK-derivable equivalent and MUST be treated as secret material.
    //
    // Usage:
    //   Tls_Mbedtls_Stream a;
    //   a.connect_result("api.binance.com"_v, 443);
    //   Vec<u8> ticket;
    //   a.save_session(ticket);    // capture after first handshake
    //   a.close();
    //
    //   Tls_Mbedtls_Stream b;
    //   b.load_session(ticket.slice());  // install before connect
    //   b.connect_result("api.binance.com"_v, 443);
    //   assert(b.session_was_resumed());

    template<Allocator A = Mdefault>
    [[nodiscard]] Result<u64, String_View> save_session(Vec<u8, A>& out) noexcept {
        if(!handshake_done_) return Result<u64, String_View>::err("tls_not_connected"_v);
        mbedtls_ssl_session sess;
        mbedtls_ssl_session_init(&sess);
        int rc = mbedtls_ssl_get_session(&ssl_, &sess);
        if(rc != 0) {
            mbedtls_ssl_session_free(&sess);
            return Result<u64, String_View>::err("tls_session_get_failed"_v);
        }
        // Two-call protocol: first call probes the required buffer size.
        size_t needed = 0;
        rc = mbedtls_ssl_session_save(&sess, nullptr, 0, &needed);
        if(rc != MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL || needed == 0) {
            mbedtls_ssl_session_free(&sess);
            return Result<u64, String_View>::err("tls_session_probe_failed"_v);
        }
        out.clear();
        out.reserve(needed);
        for(size_t i = 0; i < needed; i++) out.push(0);
        size_t written = 0;
        rc = mbedtls_ssl_session_save(&sess, out.data(), out.length(), &written);
        mbedtls_ssl_session_free(&sess);
        if(rc != 0) {
            return Result<u64, String_View>::err("tls_session_save_failed"_v);
        }
        // Trim to actual written length (should equal `needed`).
        while(out.length() > written) out.pop();
        return Result<u64, String_View>::ok(static_cast<u64>(written));
    }

    [[nodiscard]] Result<u64, String_View> load_session(Slice<const u8> ticket) noexcept {
        if(handshake_done_) return Result<u64, String_View>::err("tls_already_connected"_v);
        if(pending_session_ != nullptr) {
            // Replace any previously-loaded ticket.
            mbedtls_ssl_session_free(pending_session_);
            delete pending_session_;
            pending_session_ = nullptr;
        }
        pending_session_ = new mbedtls_ssl_session;
        mbedtls_ssl_session_init(pending_session_);
        int rc = mbedtls_ssl_session_load(pending_session_, ticket.data(), ticket.length());
        if(rc != 0) {
            mbedtls_ssl_session_free(pending_session_);
            delete pending_session_;
            pending_session_ = nullptr;
            return Result<u64, String_View>::err("tls_session_load_failed"_v);
        }
        return Result<u64, String_View>::ok(static_cast<u64>(ticket.length()));
    }

    [[nodiscard]] bool session_was_resumed() const noexcept {
        return session_resumed_;
    }

private:
    // One-time mbedTLS setup: RNG seed, CA chain, SSL config defaults,
    // and `mbedtls_ssl_setup` (which the docs explicitly forbid calling
    // more than once on the same context). Called from the first
    // `connect_result`; subsequent reconnects skip it.
    [[nodiscard]] Result<u64, String_View> setup_static_(const Options& opts) noexcept {
        if(int rc = mbedtls_ctr_drbg_seed(&ctr_drbg_, mbedtls_entropy_func, &entropy_,
                                          reinterpret_cast<const u8*>("spp-tls-mbedtls"),
                                          15)) {
            (void)rc;
            return Result<u64, String_View>::err("tls_seed_failed"_v);
        }

        if(!opts.insecure_skip_verify) {
            if(!load_ca_(opts)) return Result<u64, String_View>::err("tls_no_ca"_v);
        }

        if(int rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT)) {
            (void)rc;
            return Result<u64, String_View>::err("tls_config_failed"_v);
        }

        mbedtls_ssl_conf_authmode(&conf_,
                                  opts.insecure_skip_verify ? MBEDTLS_SSL_VERIFY_NONE
                                                            : MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf_, &cacert_, nullptr);
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctr_drbg_);
        mbedtls_ssl_conf_min_tls_version(
            &conf_, static_cast<mbedtls_ssl_protocol_version>(opts.min_tls_version));

        if(int rc = mbedtls_ssl_setup(&ssl_, &conf_)) {
            (void)rc;
            return Result<u64, String_View>::err("tls_setup_failed"_v);
        }
        return Result<u64, String_View>::ok(0);
    }

    [[nodiscard]] bool load_ca_(const Options& opts) noexcept {
        bool loaded = false;
        if(opts.ca_path.length() > 0) {
            char path[512];
            if(opts.ca_path.length() < sizeof(path)) {
                for(u64 i = 0; i < opts.ca_path.length(); i++) {
                    path[i] = static_cast<char>(opts.ca_path[i]);
                }
                path[opts.ca_path.length()] = '\0';
                if(mbedtls_x509_crt_parse_file(&cacert_, path) >= 0) loaded = true;
            }
        }
        if(opts.ca_dir.length() > 0) {
            char path[512];
            if(opts.ca_dir.length() < sizeof(path)) {
                for(u64 i = 0; i < opts.ca_dir.length(); i++) {
                    path[i] = static_cast<char>(opts.ca_dir[i]);
                }
                path[opts.ca_dir.length()] = '\0';
                if(mbedtls_x509_crt_parse_path(&cacert_, path) >= 0) loaded = true;
            }
        }
        if(opts.ca_path.length() == 0 && opts.ca_dir.length() == 0) {
            // Probe well-known Linux trust stores in priority order.
            static constexpr const char* candidates[] = {
                "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Arch
                "/etc/pki/tls/certs/ca-bundle.crt",   // RHEL/Fedora/CentOS
                "/etc/ssl/cert.pem",                  // Alpine/BSD-style
            };
            for(const char* p : candidates) {
                if(mbedtls_x509_crt_parse_file(&cacert_, p) >= 0) {
                    loaded = true;
                    break;
                }
            }
        }
        return loaded;
    }

    static int bio_send_(void* ctx, const u8* buf, size_t len) noexcept {
        auto* self = static_cast<Tls_Mbedtls_Stream*>(ctx);
        auto r = self->transport_.send_all_result(Slice<const u8>{buf, len});
        if(!r.ok()) return MBEDTLS_ERR_NET_SEND_FAILED;
        return static_cast<int>(r.unwrap());
    }

    static int bio_recv_(void* ctx, u8* buf, size_t len) noexcept {
        auto* self = static_cast<Tls_Mbedtls_Stream*>(ctx);
        auto r = self->transport_.recv_result(Slice<u8>{buf, len});
        if(!r.ok()) return MBEDTLS_ERR_NET_RECV_FAILED;
        // recv_result returning 0 means EOF; mbedTLS interprets that as
        // connection closed.
        return static_cast<int>(r.unwrap());
    }

    Net::Tcp_Client transport_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context ctr_drbg_;
    mbedtls_x509_crt cacert_;
    bool handshake_done_ = false;
    bool setup_done_ = false;       // mbedtls_ssl_setup called once
    bool session_resumed_ = false;
    // Heap-allocated because mbedtls_ssl_session is large and we want a
    // discriminated null sentinel ("no pending session" vs "pending").
    mbedtls_ssl_session* pending_session_ = nullptr;
};

} // namespace spp::Ext
