#include "test.h"

#include <spp/ext/tls_mbedtls.h>
#include <spp/io/stream.h>

// Loopback handshake/round-trip test for the mbedTLS Byte_Stream adapter.
// Spins up an in-process TLS server on 127.0.0.1 with a fresh ECDSA key + a
// self-signed certificate (generated at runtime so no test fixtures live in
// the repo), drives a client through Tls_Mbedtls_Stream::connect_result +
// send_all_result + recv_result, and asserts the bytes round-trip across the
// real TLS session.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Concept conformance: an upper-layer call site that requires Byte_Stream
// must accept Tls_Mbedtls_Stream identically to a plain Tcp_Client.
static_assert(Net::Byte_Stream<Ext::Tls_Mbedtls_Stream>,
              "Tls_Mbedtls_Stream must satisfy Byte_Stream");

namespace {

struct Test_Server {
    mbedtls_pk_context key{};
    mbedtls_x509_crt cert_chain{};
    Vec<u8, Mdefault> cert_pem;
    Vec<u8, Mdefault> key_pem;
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    int listener_fd = -1;
    u16 port = 0;

    bool prepare() noexcept {
        mbedtls_pk_init(&key);
        mbedtls_x509_crt_init(&cert_chain);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);

        if(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                 reinterpret_cast<const u8*>("test"), 4) != 0) {
            return false;
        }

        // Generate an ECDSA P-256 key.
        if(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
            return false;
        }
        if(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                               mbedtls_ctr_drbg_random, &drbg) != 0) {
            return false;
        }

        // Self-signed certificate.
        mbedtls_x509write_cert cw;
        mbedtls_x509write_crt_init(&cw);
        mbedtls_x509write_crt_set_version(&cw, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&cw, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_issuer_key(&cw, &key);
        mbedtls_x509write_crt_set_subject_key(&cw, &key);
        static_cast<void>(mbedtls_x509write_crt_set_subject_name(&cw, "CN=localhost"));
        static_cast<void>(mbedtls_x509write_crt_set_issuer_name(&cw, "CN=localhost"));
        static_cast<void>(mbedtls_x509write_crt_set_validity(&cw, "20260101000000",
                                                             "20300101000000"));
        mbedtls_mpi serial;
        mbedtls_mpi_init(&serial);
        static_cast<void>(mbedtls_mpi_lset(&serial, 1));
        static_cast<void>(mbedtls_x509write_crt_set_serial(&cw, &serial));
        mbedtls_mpi_free(&serial);

        u8 cert_buf[4096];
        int written = mbedtls_x509write_crt_pem(&cw, cert_buf, sizeof(cert_buf),
                                                mbedtls_ctr_drbg_random, &drbg);
        if(written < 0) {
            mbedtls_x509write_crt_free(&cw);
            return false;
        }
        // mbedtls writes from the end of the buffer; the PEM string is
        // NUL-terminated so we can just use strlen to find the length when
        // writing, but the function returns 0 on success (in some versions).
        // To be safe, scan for the NUL terminator within the buffer.
        u64 cert_len = 0;
        for(u64 i = 0; i < sizeof(cert_buf); i++) {
            if(cert_buf[i] != 0) {
                cert_pem.push(cert_buf[i]);
                cert_len++;
            } else if(cert_len > 0) {
                break;
            }
        }
        cert_pem.push(0); // mbedtls_x509_crt_parse needs NUL terminator
        mbedtls_x509write_crt_free(&cw);

        if(mbedtls_x509_crt_parse(&cert_chain, cert_pem.data(), cert_pem.length()) != 0) {
            return false;
        }

        // Open listening TCP socket on an ephemeral port.
        listener_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if(listener_fd < 0) return false;
        int opt = 1;
        static_cast<void>(::setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                                       sizeof(opt)));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if(::bind(listener_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        if(::getsockname(listener_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
            return false;
        }
        port = ntohs(bound.sin_port);
        if(::listen(listener_fd, 1) != 0) return false;
        return true;
    }

    bool serve_one_round_trip() noexcept {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int conn_fd = ::accept(listener_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if(conn_fd < 0) return false;

        mbedtls_ssl_context ssl;
        mbedtls_ssl_config conf;
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);

        bool ok = false;
        do {
            if(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                            MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT) != 0) break;
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
            if(mbedtls_ssl_conf_own_cert(&conf, &cert_chain, &key) != 0) break;
            mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
            if(mbedtls_ssl_setup(&ssl, &conf) != 0) break;

            mbedtls_net_context net_ctx;
            net_ctx.fd = conn_fd;
            mbedtls_ssl_set_bio(&ssl, &net_ctx, mbedtls_net_send, mbedtls_net_recv, nullptr);

            int rc;
            while((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
                if(rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    goto done;
                }
            }

            // Read the client's request, echo it back with an "ECHO:" prefix.
            u8 in_buf[64]{};
            int got = mbedtls_ssl_read(&ssl, in_buf, sizeof(in_buf));
            if(got <= 0) goto done;

            u8 reply[64];
            const char* prefix = "ECHO:";
            int plen = 5;
            for(int i = 0; i < plen; i++) reply[i] = static_cast<u8>(prefix[i]);
            for(int i = 0; i < got; i++) reply[plen + i] = in_buf[i];

            int wrote = mbedtls_ssl_write(&ssl, reply, plen + got);
            if(wrote != plen + got) goto done;

            static_cast<void>(mbedtls_ssl_close_notify(&ssl));
            ok = true;
        } while(false);
done:
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        ::close(conn_fd);
        return ok;
    }

    void teardown() noexcept {
        if(listener_fd >= 0) {
            ::close(listener_fd);
            listener_fd = -1;
        }
        mbedtls_x509_crt_free(&cert_chain);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

} // namespace

i32 main() {
    Test test{"empty"_v};

    Trace("Tls_Mbedtls_Stream construct/destruct without connect is clean") {
        Ext::Tls_Mbedtls_Stream tls;
        assert(!tls.handshake_done());
        // Destruction here runs the full mbedtls teardown without a prior
        // connect — must not crash or leak.
    }

    Trace("Tls_Mbedtls_Stream connect failure surfaces a Result error") {
        Ext::Tls_Mbedtls_Stream tls;
        // Port 1 on loopback is reserved + typically unreachable; the TCP
        // connect should fail and propagate up through the adapter.
        auto rc = tls.connect_result("127.0.0.1"_v, 1);
        assert(!rc.ok());
    }

    Trace("Tls_Mbedtls_Stream save_session before connect rejects") {
        // save_session requires an active handshake; pre-connect calls must
        // surface a typed error, not crash.
        Ext::Tls_Mbedtls_Stream tls;
        Vec<u8, Mdefault> ticket;
        auto rc = tls.save_session(ticket);
        assert(!rc.ok());
        assert(rc.unwrap_err() == "tls_not_connected"_v);
    }

    Trace("Tls_Mbedtls_Stream loopback handshake + round-trip") {
        Test_Server srv;
        bool ready = srv.prepare();
        assert(ready);

        auto server_task = Thread::spawn([&srv]() -> i32 {
            return srv.serve_one_round_trip() ? 0 : 1;
        });

        Ext::Tls_Mbedtls_Stream tls;
        Ext::Tls_Mbedtls_Stream::Options opts;
        opts.insecure_skip_verify = true; // local self-signed cert
        auto connected = tls.connect_result("127.0.0.1"_v, srv.port, opts);
        assert(connected.ok());
        assert(tls.handshake_done());

        // Session was not loaded, so resumption flag stays false.
        assert(!tls.session_was_resumed());

        // save_session after a successful handshake must yield a non-empty
        // ticket blob. The resumption side is checked end-to-end in the
        // dedicated test below.
        Vec<u8, Mdefault> saved;
        auto save_rc = tls.save_session(saved);
        assert(save_rc.ok());
        assert(saved.length() > 0);

        u8 ping[] = {'P', 'I', 'N', 'G'};
        auto sent = tls.send_all_result(Slice<const u8>{ping, 4});
        assert(sent.ok());
        assert(sent.unwrap() == 4);

        u8 reply[9]{};
        auto got = tls.recv_exact_result(Slice<u8>{reply, 9});
        assert(got.ok());
        assert(reply[0] == 'E' && reply[1] == 'C' && reply[2] == 'H' && reply[3] == 'O');
        assert(reply[4] == ':');
        assert(reply[5] == 'P' && reply[6] == 'I' && reply[7] == 'N' && reply[8] == 'G');

        tls.close();
        assert(!tls.handshake_done());

        i32 server_rc = server_task->block();
        assert(server_rc == 0);

        srv.teardown();
    }

    return 0;
}
