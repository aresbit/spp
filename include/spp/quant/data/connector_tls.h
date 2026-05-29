#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/data/connector.h"

// =========================================================================
// OpenSSL conditional inclusion
// =========================================================================
// When SPP_USE_OPENSSL is defined, link with -lssl -lcrypto.
// When not defined, TLS functions will return errors with clear messages.
// =========================================================================

#ifdef SPP_USE_OPENSSL
#ifdef SPP_OS_LINUX
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif
#endif

namespace spp::quant {

// =========================================================================
// TLSContext -- wraps OpenSSL SSL_CTX for encrypted connections
// =========================================================================
// Thread-safe: create one TLSContext per application (or per connection pool)
// and share it across connections.

struct TLSContext {
#ifdef SPP_USE_OPENSSL
    void* ssl_ctx_; // SSL_CTX*
#else
    void* ssl_ctx_ = null;
#endif

    // -----------------------------------------------------------------
    // create_client -- create a TLS client context
    // -----------------------------------------------------------------
    static TLSContext create_client() noexcept {
        TLSContext ctx;
#ifdef SPP_USE_OPENSSL
        ctx.ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx.ssl_ctx_) {
            // Set minimum TLS version to 1.2
            SSL_CTX_set_min_proto_version(static_cast<SSL_CTX*>(ctx.ssl_ctx_), TLS1_2_VERSION);
            // Default: verify server certificate
            SSL_CTX_set_verify(static_cast<SSL_CTX*>(ctx.ssl_ctx_), SSL_VERIFY_PEER, null);
        }
#else
        (void)ctx;
#endif
        return ctx;
    }

    // -----------------------------------------------------------------
    // load_ca_cert -- load CA certificates from a file
    // -----------------------------------------------------------------
    bool load_ca_cert(String_View ca_path) noexcept {
#ifdef SPP_USE_OPENSSL
        if (!ssl_ctx_) return false;
        String ca_str{ca_path.length() + 1};
        ca_str.set_length(ca_path.length() + 1);
        for (u64 i = 0; i < ca_path.length(); i++) ca_str[i] = ca_path[i];
        ca_str[ca_path.length()] = '\0';

        i32 ret = SSL_CTX_load_verify_locations(
            static_cast<SSL_CTX*>(ssl_ctx_),
            reinterpret_cast<const char*>(ca_str.data()),
            null);
        return ret == 1;
#else
        (void)ca_path;
        return false;
#endif
    }

    // -----------------------------------------------------------------
    // load_ca_cert_default -- use system CA bundle
    // -----------------------------------------------------------------
    bool load_ca_cert_default() noexcept {
#ifdef SPP_USE_OPENSSL
        if (!ssl_ctx_) return false;
        // On Linux, default paths are typically /etc/ssl/certs/ or similar
        i32 ret = SSL_CTX_set_default_verify_paths(static_cast<SSL_CTX*>(ssl_ctx_));
        return ret == 1;
#else
        return false;
#endif
    }

    // -----------------------------------------------------------------
    // set_no_verify -- disable server certificate verification
    // [UNSPECIFIED] Security: only use for testing/internal networks
    // -----------------------------------------------------------------
    void set_no_verify() noexcept {
#ifdef SPP_USE_OPENSSL
        if (ssl_ctx_) {
            SSL_CTX_set_verify(static_cast<SSL_CTX*>(ssl_ctx_), SSL_VERIFY_NONE, null);
        }
#endif
    }

    // -----------------------------------------------------------------
    // destroy -- free the TLS context
    // -----------------------------------------------------------------
    void destroy() noexcept {
#ifdef SPP_USE_OPENSSL
        if (ssl_ctx_) {
            SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
            ssl_ctx_ = null;
        }
#endif
    }
};

// =========================================================================
// TLSConnection -- per-connection TLS state
// =========================================================================

struct TLSConnection {
#ifdef SPP_USE_OPENSSL
    void* ssl_ = null;
#else
    void* ssl_ = null;
#endif
    bool connected_ = false;
    u64 socket_fd_ = ~u64{0};

    // -----------------------------------------------------------------
    // connect -- upgrade an existing TCP socket to TLS
    //
    // Performs TLS handshake (client-side).
    // hostname: for SNI (Server Name Indication) and certificate verification
    // -----------------------------------------------------------------
    static Opt<TLSConnection> connect(u64 socket_fd, String_View hostname, TLSContext& ctx) noexcept {
#ifdef SPP_USE_OPENSSL
        if (!ctx.ssl_ctx_) return {};

        SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx.ssl_ctx_));
        if (!ssl) return {};

        SSL_set_fd(ssl, static_cast<i32>(socket_fd));

        // Set SNI hostname
        String host_str{hostname.length() + 1};
        host_str.set_length(hostname.length() + 1);
        for (u64 i = 0; i < hostname.length(); i++) host_str[i] = hostname[i];
        host_str[hostname.length()] = '\0';
        SSL_set_tlsext_host_name(ssl, reinterpret_cast<const char*>(host_str.data()));

        i32 ret = SSL_connect(ssl);
        if (ret != 1) {
            SSL_free(ssl);
            return {};
        }

        TLSConnection conn;
        conn.ssl_ = ssl;
        conn.connected_ = true;
        conn.socket_fd_ = socket_fd;
        return Opt<TLSConnection>{spp::move(conn)};
#else
        (void)socket_fd; (void)hostname; (void)ctx;
        return {};
#endif
    }

    // -----------------------------------------------------------------
    // send -- send data over TLS
    // -----------------------------------------------------------------
    i64 send(const u8* data, u64 size) noexcept {
#ifdef SPP_USE_OPENSSL
        if (!ssl_ || !connected_) return -1;
        i32 ret = SSL_write(static_cast<SSL*>(ssl_), data, static_cast<i32>(size));
        return ret > 0 ? static_cast<i64>(ret) : -1;
#else
        (void)data; (void)size;
        return -1;
#endif
    }

    // -----------------------------------------------------------------
    // recv -- receive data over TLS with timeout
    //
    // timeout_ms: 0 = non-blocking poll, ~u64{0} = infinite
    // -----------------------------------------------------------------
    i64 recv(u8* buffer, u64 size, u64 timeout_ms) noexcept {
#ifdef SPP_USE_OPENSSL
        if (!ssl_ || !connected_) return -1;

        // Check if data is already buffered in OpenSSL
        i32 pending = SSL_pending(static_cast<SSL*>(ssl_));
        if (pending > 0) {
            i32 to_read = pending < static_cast<i32>(size) ? pending : static_cast<i32>(size);
            i32 ret = SSL_read(static_cast<SSL*>(ssl_), buffer, to_read);
            return ret > 0 ? static_cast<i64>(ret) : -1;
        }

        // Poll underlying socket with select()
        if (timeout_ms > 0 && socket_fd_ != ~u64{0}) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(static_cast<i32>(socket_fd_), &rfds);
            struct timeval tv;
            tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
            tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
            i32 sel_ret = ::select(static_cast<i32>(socket_fd_) + 1, &rfds, null, null, &tv);
            if (sel_ret <= 0) return sel_ret; // 0 = timeout, -1 = error
        }

        i32 ret = SSL_read(static_cast<SSL*>(ssl_), buffer, static_cast<i32>(size));
        if (ret <= 0) {
            i32 err = SSL_get_error(static_cast<SSL*>(ssl_), ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
            return -1;
        }
        return static_cast<i64>(ret);
#else
        (void)buffer; (void)size; (void)timeout_ms;
        return -1;
#endif
    }

    // -----------------------------------------------------------------
    // close -- perform orderly TLS shutdown (does NOT close TCP socket)
    // -----------------------------------------------------------------
    void close() noexcept {
#ifdef SPP_USE_OPENSSL
        if (ssl_) {
            if (connected_) SSL_shutdown(static_cast<SSL*>(ssl_));
            SSL_free(static_cast<SSL*>(ssl_));
            ssl_ = null;
        }
#endif
        connected_ = false;
    }

    // -----------------------------------------------------------------
    // is_connected -- check if TLS handshake was successful
    // -----------------------------------------------------------------
    [[nodiscard]] bool is_connected() const noexcept { return connected_; }
};

// =========================================================================
// TLSSocket -- unified socket abstraction (plain TCP or TLS)
// =========================================================================
// Pass this to tls_socket_* functions for transparent TCP/TLS I/O.

struct TLSSocket {
    u64 fd_ = ~u64{0};
    void* ssl_ = null;
    bool is_tls_ = false;
};

// =========================================================================
// tls_socket_* functions -- drop-in replacements for plain socket I/O
// =========================================================================
// Use these instead of WebSocketConnector::socket_send / socket_recv when
// TLS is active.  When is_tls_ is false, they delegate to plain send/recv.

namespace tls_socket {

[[nodiscard]] inline i64 send(TLSSocket& sock, const u8* data, u64 size) noexcept {
    if (sock.is_tls_ && sock.ssl_) {
#ifdef SPP_USE_OPENSSL
        i32 ret = SSL_write(static_cast<SSL*>(sock.ssl_), data, static_cast<i32>(size));
        return ret > 0 ? static_cast<i64>(ret) : -1;
#else
        return -1;
#endif
    }
#ifdef SPP_OS_LINUX
    auto n = ::send(static_cast<i32>(sock.fd_), data, size, MSG_NOSIGNAL);
    return n >= 0 ? n : -1;
#else
    (void)sock; (void)data; (void)size;
    return -1;
#endif
}

[[nodiscard]] inline i64 recv(TLSSocket& sock, u8* buffer, u64 size, u64 timeout_ms) noexcept {
    if (sock.is_tls_ && sock.ssl_) {
#ifdef SPP_USE_OPENSSL
        // Check OpenSSL internal buffer first
        i32 pending = SSL_pending(static_cast<SSL*>(sock.ssl_));
        if (pending > 0) {
            i32 to_read = pending < static_cast<i32>(size) ? pending : static_cast<i32>(size);
            i32 ret = SSL_read(static_cast<SSL*>(sock.ssl_), buffer, to_read);
            return ret > 0 ? static_cast<i64>(ret) : -1;
        }
#endif
    }

    // Poll socket (works for both plain TCP and TLS underlying fd)
#ifdef SPP_OS_LINUX
    if (timeout_ms > 0 && sock.fd_ != ~u64{0}) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(static_cast<i32>(sock.fd_), &rfds);
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        i32 ret = ::select(static_cast<i32>(sock.fd_) + 1, &rfds, null, null, &tv);
        if (ret <= 0) return ret;
    }
#endif

    if (sock.is_tls_ && sock.ssl_) {
#ifdef SPP_USE_OPENSSL
        i32 ret = SSL_read(static_cast<SSL*>(sock.ssl_), buffer, static_cast<i32>(size));
        if (ret <= 0) {
            i32 err = SSL_get_error(static_cast<SSL*>(sock.ssl_), ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
            return -1;
        }
        return static_cast<i64>(ret);
#else
        return -1;
#endif
    }

#ifdef SPP_OS_LINUX
    auto n = ::recv(static_cast<i32>(sock.fd_), buffer, size, 0);
    return n >= 0 ? n : -1;
#else
    (void)buffer; (void)size;
    return -1;
#endif
}

inline void close(TLSSocket& sock) noexcept {
    if (sock.is_tls_ && sock.ssl_) {
#ifdef SPP_USE_OPENSSL
        SSL_shutdown(static_cast<SSL*>(sock.ssl_));
        SSL_free(static_cast<SSL*>(sock.ssl_));
#endif
        sock.ssl_ = null;
        sock.is_tls_ = false;
    }
    if (sock.fd_ != ~u64{0}) {
#ifdef SPP_OS_LINUX
        ::close(static_cast<i32>(sock.fd_));
#endif
        sock.fd_ = ~u64{0};
    }
}

// Upgrade a plain TCP socket to TLS -- mutate sock in place
inline bool upgrade(TLSSocket& sock, String_View hostname, TLSContext& ctx) noexcept {
#ifdef SPP_USE_OPENSSL
    if (!ctx.ssl_ctx_ || sock.fd_ == ~u64{0}) return false;

    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx.ssl_ctx_));
    if (!ssl) return false;

    SSL_set_fd(ssl, static_cast<i32>(sock.fd_));

    String host_str{hostname.length() + 1};
    host_str.set_length(hostname.length() + 1);
    for (u64 i = 0; i < hostname.length(); i++) host_str[i] = hostname[i];
    host_str[hostname.length()] = '\0';
    SSL_set_tlsext_host_name(ssl, reinterpret_cast<const char*>(host_str.data()));

    i32 ret = SSL_connect(ssl);
    if (ret != 1) {
        SSL_free(ssl);
        return false;
    }

    sock.ssl_ = ssl;
    sock.is_tls_ = true;
    return true;
#else
    (void)sock; (void)hostname; (void)ctx;
    return false;
#endif
}

} // namespace tls_socket

// =========================================================================
// SecureWebSocketConnector -- WebSocket with wss:// TLS support
// =========================================================================
// Extends WebSocketConnector to properly handle wss:// URLs.
// Overrides connect() to perform TLS upgrade after TCP connect and
// routes all I/O through TLSSocket.

struct SecureWebSocketConnector : WebSocketConnector {
    TLSSocket tls_sock_;
    TLSContext tls_ctx_;

    SecureWebSocketConnector() noexcept {
        name_ = "SecureWebSocketConnector"_v;
        tls_ctx_ = TLSContext::create_client();
    }

    ~SecureWebSocketConnector() override {
        disconnect();
        tls_ctx_.destroy();
    }

    // ==================================================================
    // Override connect to support wss://
    // ==================================================================
    bool connect() override {
#ifdef SPP_OS_LINUX
        String_View url = endpoint_url_;
        bool use_tls = false;
        u64 scheme_end = 0;

        if (sv::starts_with(url, "wss://"_v)) { use_tls = true; scheme_end = 6; }
        else if (sv::starts_with(url, "ws://"_v)) { scheme_end = 5; }
        else { scheme_end = 0; }

        String_View rest = url.sub(scheme_end, url.length());

        // Parse host:port/path from rest
        u64 host_end = 0, port_start_val = 0;
        u16 port = use_tls ? 443 : 80;

        for (u64 i = 0; i < rest.length(); i++) {
            if (static_cast<char>(rest[i]) == ':') { host_end = i; port_start_val = i + 1; }
            if (static_cast<char>(rest[i]) == '/') { if (host_end == 0) host_end = i; break; }
        }
        if (host_end == 0) host_end = rest.length();
        String_View host = rest.sub(0, host_end);

        if (port_start_val > 0) {
            u64 pe = port_start_val;
            while (pe < rest.length() && rest[pe] >= '0' && rest[pe] <= '9') pe++;
            port = 0;
            for (u64 i = port_start_val; i < pe; i++)
                port = port * 10 + static_cast<u16>(rest[i] - '0');
        }

        u64 ps = 0;
        for (u64 i = 0; i < rest.length(); i++) {
            if (static_cast<char>(rest[i]) == '/') { ps = i; break; }
        }
        String_View path = "/"_v;
        if (ps > 0) path = rest.sub(ps, rest.length());

        // ---- TCP connect ----
        u64 fd = create_socket();
        if (fd == ~u64{0}) return false;
        if (!socket_connect(fd, host, port, 10000)) {
            socket_close(fd);
            return false;
        }

        tls_sock_.fd_ = fd;
        tls_sock_.is_tls_ = false;
        tls_sock_.ssl_ = null;

        // ---- TLS upgrade if wss:// ----
        if (use_tls) {
#ifndef SPP_USE_OPENSSL
            socket_close(fd);
            tls_sock_.fd_ = ~u64{0};
            return false; // TLS not available -- define SPP_USE_OPENSSL and link -lssl -lcrypto
#else
            if (!tls_ctx_.ssl_ctx_) {
                socket_close(fd);
                tls_sock_.fd_ = ~u64{0};
                return false;
            }
            if (!tls_socket::upgrade(tls_sock_, host, tls_ctx_)) {
                socket_close(fd);
                tls_sock_.fd_ = ~u64{0};
                return false;
            }
#endif
        }

        // ---- WebSocket handshake (using TLS-aware I/O) ----
        if (!ws_handshake_tls(host, path)) {
            tls_socket::close(tls_sock_);
            return false;
        }

        socket_fd_ = tls_sock_.fd_;
        connected_ = true;

        // Re-seed PRNG
        ws_detail::ws_prng_state[0] = 0xdeadbeefcafebabeULL ^ static_cast<u64>(
            reinterpret_cast<uintptr_t>(this));
        return true;
#else
        return false;
#endif
    }

    // ==================================================================
    // Override disconnect
    // ==================================================================
    void disconnect() override {
        if (connected_ && tls_sock_.fd_ != ~u64{0}) {
            u8 close_frame[4] = {0x88, 0x02, 0x03, 0xE8};
            tls_socket::send(tls_sock_, close_frame, 4);
        }
        tls_socket::close(tls_sock_);
        socket_fd_ = ~u64{0};
        connected_ = false;
        subscribed_.clear();
        bar_buffer_.clear();
        quote_buffer_.clear();
    }

    // ==================================================================
    // TLS-aware WebSocket handshake (replaces ws_handshake)
    // ==================================================================
    bool ws_handshake_tls(String_View host, String_View path) noexcept {
        // Generate key
        u8 key_bytes[16];
        for (i32 i = 0; i < 16; i++)
            key_bytes[i] = static_cast<u8>(ws_detail::ws_prng_next() & 0xFF);
        String<> client_key = ws_detail::ws_base64_encode(Slice<const u8>{key_bytes, 16});

        // Build HTTP upgrade request
        Vec<u8> req;
        req.reserve(512);

        auto push_sv = [&req](String_View s) {
            for (u64 i = 0; i < s.length(); i++) req.push(s[i]);
        };

        push_sv("GET "_v); push_sv(path); push_sv(" HTTP/1.1\r\n"_v);
        push_sv("Host: "_v); push_sv(host); push_sv("\r\n"_v);
        push_sv("Upgrade: websocket\r\n"_v);
        push_sv("Connection: Upgrade\r\n"_v);
        push_sv("Sec-WebSocket-Key: "_v);
        push_sv(client_key.view());
        push_sv("\r\n"_v);
        push_sv("Sec-WebSocket-Version: 13\r\n"_v);
        if (!api_key_.empty()) {
            push_sv("X-API-Key: "_v); push_sv(api_key_); push_sv("\r\n"_v);
        }
        push_sv("\r\n"_v);

        auto n = tls_socket::send(tls_sock_, req.data(), req.length());
        if (n < 0) return false;

        // Read response
        u8 tmp[4096];
        u64 total_read = 0;
        for (u64 attempt = 0; attempt < 5 && total_read < sizeof(tmp); attempt++) {
            i64 r = tls_socket::recv(tls_sock_, tmp + total_read,
                                     sizeof(tmp) - total_read, 3000);
            if (r <= 0) break;
            total_read += static_cast<u64>(r);
            if (total_read >= 4) {
                bool done = false;
                for (u64 i = 0; i + 3 < total_read; i++) {
                    if (tmp[i] == '\r' && tmp[i+1] == '\n' &&
                        tmp[i+2] == '\r' && tmp[i+3] == '\n') { done = true; break; }
                }
                if (done) break;
            }
        }

        String_View response{tmp, total_read};
        if (response.length() >= 12 &&
            response.sub(0, 9) == "HTTP/1.1 "_v &&
            response.sub(9, 12) == "101"_v) {
            return true;
        }
        return false;
    }

    // ==================================================================
    // TLS-aware WebSocket send (overrides ws_send)
    // ==================================================================
    bool ws_send(String_View message) noexcept {
        send_buffer_.clear();
        u64 msg_len = message.length();
        u8 mask[4];
        for (i32 i = 0; i < 4; i++)
            mask[i] = static_cast<u8>(ws_detail::ws_prng_next() & 0xFF);

        send_buffer_.push(0x81); // FIN + text
        if (msg_len <= 125) {
            send_buffer_.push(static_cast<u8>(msg_len | 0x80));
        } else if (msg_len <= 65535) {
            send_buffer_.push(static_cast<u8>(126 | 0x80));
            send_buffer_.push(static_cast<u8>((msg_len >> 8) & 0xFF));
            send_buffer_.push(static_cast<u8>(msg_len & 0xFF));
        } else {
            send_buffer_.push(static_cast<u8>(127 | 0x80));
            for (i32 i = 7; i >= 0; i--)
                send_buffer_.push(static_cast<u8>((msg_len >> (i*8)) & 0xFF));
        }
        for (i32 i = 0; i < 4; i++) send_buffer_.push(mask[i]);
        for (u64 i = 0; i < msg_len; i++)
            send_buffer_.push(static_cast<u8>(message[i] ^ mask[i % 4]));

        auto n = tls_socket::send(tls_sock_, send_buffer_.data(), send_buffer_.length());
        return n == static_cast<i64>(send_buffer_.length());
    }

    // ==================================================================
    // TLS-aware WebSocket recv (overrides ws_recv)
    // ==================================================================
    Opt<String<>> ws_recv(u64 timeout_ms = 5000) noexcept {
        recv_buffer_.clear();
        u8 header[2];
        i64 r = tls_socket::recv(tls_sock_, header, 2, timeout_ms);
        if (r < 2) return {};

        u8 opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        u64 payload_len = header[1] & 0x7F;

        if (payload_len == 126) {
            u8 ext[2]; r = tls_socket::recv(tls_sock_, ext, 2, timeout_ms);
            if (r < 2) return {};
            payload_len = (static_cast<u64>(ext[0]) << 8) | static_cast<u64>(ext[1]);
        } else if (payload_len == 127) {
            u8 ext[8]; r = tls_socket::recv(tls_sock_, ext, 8, timeout_ms);
            if (r < 8) return {};
            payload_len = 0;
            for (i32 i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | static_cast<u64>(ext[i]);
        }

        u8 mask_key[4] = {};
        if (masked) {
            r = tls_socket::recv(tls_sock_, mask_key, 4, timeout_ms);
            if (r < 4) return {};
        }

        if (payload_len > 0) {
            recv_buffer_.reserve(payload_len);
            for (u64 i = 0; i < payload_len; i++) recv_buffer_.push(0);
            u64 total = 0;
            while (total < payload_len) {
                u64 remaining = payload_len - total;
                r = tls_socket::recv(tls_sock_, recv_buffer_.data() + total,
                                     remaining, timeout_ms);
                if (r <= 0) return {};
                total += static_cast<u64>(r);
            }
        }

        if (opcode == 0x08) { connected_ = false; return {}; }
        if (opcode == 0x09) {
            u8 pong[2] = {0x8A, 0x00};
            tls_socket::send(tls_sock_, pong, 2);
            return ws_recv(timeout_ms);
        }
        if (opcode == 0x0A) { return ws_recv(timeout_ms); }
        if (opcode != 0x01 && opcode != 0x02) return {};

        if (masked && payload_len > 0) {
            for (u64 i = 0; i < payload_len; i++)
                recv_buffer_[i] ^= mask_key[i % 4];
        }

        String<> result{payload_len};
        result.set_length(payload_len);
        for (u64 i = 0; i < payload_len; i++) result[i] = recv_buffer_[i];
        return Opt<String<>>{spp::move(result)};
    }

    // ==================================================================
    // poll -- read available messages (uses TLS-aware recv)
    // ==================================================================
    u64 poll(u64 timeout_ms = 0) noexcept {
        if (!connected_) return 0;
        u64 count = 0;
        for (;;) {
            auto msg = ws_recv(timeout_ms);
            if (!msg.ok()) break;
            parse_message(msg->view());
            count++;
            if (timeout_ms == 0) break;
        }
        return count;
    }
};

} // namespace spp::quant
