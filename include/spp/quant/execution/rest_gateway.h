#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/execution/order.h"
#include "spp/quant/backtest/event.h"
#include "spp/quant/data/connector.h"
#include "spp/quant/base/date.h"
#include "spp/quant/base/currency.h"

#ifdef SPP_OS_LINUX
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// Forward TLS types for HTTPS (available from connector_tls.h)
#ifdef SPP_USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace spp::quant::execution {

using spp::quant::backtest::OrderSide;
using spp::quant::backtest::OrderType;
using spp::quant::backtest::OrderStatus;

// =========================================================================
// SHA256 -- FIPS 180-4, used for HMAC-SHA256 (Binance auth)
// =========================================================================
// Implementation follows the same pattern as SHA1 in connector.h.
// Reference: FIPS PUB 180-4, Section 6.2

namespace sha256_detail {

static const u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

struct SHA256_CTX {
    u32 state[8] = {};
    u64 bit_count = 0;
    u8 buffer[64] = {};
    u64 buffer_len = 0;
};

inline u32 sha256_rotr(u32 x, i32 n) noexcept { return (x >> n) | (x << (32 - n)); }

inline void sha256_init(SHA256_CTX* ctx) noexcept {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bit_count = 0; ctx->buffer_len = 0;
}

inline void sha256_transform(u32 state[8], const u8 block[64]) noexcept {
    u32 w[64];
    for (i32 i = 0; i < 16; i++)
        w[i] = (static_cast<u32>(block[i*4]) << 24) |
               (static_cast<u32>(block[i*4+1]) << 16) |
               (static_cast<u32>(block[i*4+2]) << 8) |
               static_cast<u32>(block[i*4+3]);
    for (i32 i = 16; i < 64; i++) {
        u32 s0 = sha256_rotr(w[i-15], 7) ^ sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = sha256_rotr(w[i-2], 17) ^ sha256_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 a = state[0], b = state[1], c = state[2], d = state[3],
        e = state[4], f = state[5], g = state[6], h = state[7];

    for (i32 i = 0; i < 64; i++) {
        u32 S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 temp1 = h + S1 + ch + sha256_k[i] + w[i];
        u32 S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

inline void sha256_update(SHA256_CTX* ctx, const u8* data, u64 len) noexcept {
    ctx->bit_count += len * 8;
    u64 i = 0;
    while (i < len) {
        u64 space = 64 - ctx->buffer_len, chunk = len - i;
        if (chunk > space) chunk = space;
        for (u64 j = 0; j < chunk; j++) ctx->buffer[ctx->buffer_len + j] = data[i + j];
        ctx->buffer_len += chunk; i += chunk;
        if (ctx->buffer_len == 64) {
            sha256_transform(ctx->state, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

inline void sha256_final(SHA256_CTX* ctx, u8 digest[32]) noexcept {
    ctx->buffer[ctx->buffer_len++] = 0x80;
    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < 64) ctx->buffer[ctx->buffer_len++] = 0;
        sha256_transform(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
    }
    while (ctx->buffer_len < 56) ctx->buffer[ctx->buffer_len++] = 0;
    for (i32 i = 7; i >= 0; i--)
        ctx->buffer[ctx->buffer_len++] = static_cast<u8>((ctx->bit_count >> (i * 8)) & 0xFF);
    sha256_transform(ctx->state, ctx->buffer);
    for (i32 i = 0; i < 8; i++) {
        digest[i*4]   = static_cast<u8>((ctx->state[i] >> 24) & 0xFF);
        digest[i*4+1] = static_cast<u8>((ctx->state[i] >> 16) & 0xFF);
        digest[i*4+2] = static_cast<u8>((ctx->state[i] >> 8) & 0xFF);
        digest[i*4+3] = static_cast<u8>(ctx->state[i] & 0xFF);
    }
}

// Compute SHA256 hash of data, return 32-byte digest
inline void sha256_hash(Slice<const u8> data, u8 digest[32]) noexcept {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data.data(), data.length());
    sha256_final(&ctx, digest);
}

} // namespace sha256_detail

// =========================================================================
// HMAC-SHA256 -- RFC 2104
// =========================================================================
// HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
// where K' is K padded/truncated to 64 bytes (SHA256 block size)

namespace hmac_detail {

constexpr u64 sha256_block_size = 64;

inline void hmac_sha256(Slice<const u8> key, Slice<const u8> message,
                         u8 digest[32]) noexcept {
    using namespace sha256_detail;

    u8 key_block[sha256_block_size] = {};

    // If key is longer than block size, hash it first
    if (key.length() > sha256_block_size) {
        u8 hashed_key[32];
        sha256_hash(key, hashed_key);
        for (u64 i = 0; i < 32; i++) key_block[i] = hashed_key[i];
        // Remaining 32 bytes are already zero
    } else {
        for (u64 i = 0; i < key.length(); i++) key_block[i] = key[i];
    }

    // Inner: H((key ^ ipad) || message)
    u8 ipad[sha256_block_size];
    for (u64 i = 0; i < sha256_block_size; i++) ipad[i] = key_block[i] ^ 0x36;

    SHA256_CTX inner_ctx;
    sha256_init(&inner_ctx);
    sha256_update(&inner_ctx, ipad, sha256_block_size);
    sha256_update(&inner_ctx, message.data(), message.length());
    u8 inner_hash[32];
    sha256_final(&inner_ctx, inner_hash);

    // Outer: H((key ^ opad) || inner_hash)
    u8 opad[sha256_block_size];
    for (u64 i = 0; i < sha256_block_size; i++) opad[i] = key_block[i] ^ 0x5c;

    SHA256_CTX outer_ctx;
    sha256_init(&outer_ctx);
    sha256_update(&outer_ctx, opad, sha256_block_size);
    sha256_update(&outer_ctx, inner_hash, 32);
    sha256_final(&outer_ctx, digest);
}

// Hex encode binary data to lowercase hex string
inline String<> hex_encode(Slice<const u8> data) noexcept {
    static const char hex_chars[] = "0123456789abcdef";
    u64 out_len = data.length() * 2;
    String<> s{out_len};
    s.set_length(out_len);
    for (u64 i = 0; i < data.length(); i++) {
        s[i*2]     = static_cast<u8>(hex_chars[(data[i] >> 4) & 0x0F]);
        s[i*2 + 1] = static_cast<u8>(hex_chars[data[i] & 0x0F]);
    }
    return s;
}

// One-shot HMAC-SHA256 returning hex-encoded string
inline String<> hmac_sha256_hex(String_View key, String_View message) noexcept {
    u8 digest[32];
    hmac_sha256(Slice<const u8>{key.data(), key.length()},
                Slice<const u8>{message.data(), message.length()},
                digest);
    return hex_encode(Slice<const u8>{digest, 32});
}

} // namespace hmac_detail

// =========================================================================
// HTTP helpers -- URL parsing, request building, response parsing
// =========================================================================

namespace http_detail {

struct URLParts {
    String_View scheme_;   // "http" or "https"
    String_View host_;
    u16 port_ = 80;
    String_View path_;     // includes leading "/"
};

// Parse URL into components
// Supported: scheme://host[:port][/path]
inline URLParts parse_url(String_View url) noexcept {
    URLParts parts;

    u64 scheme_end = 0;
    if (sv::starts_with(url, "https://"_v)) {
        parts.scheme_ = "https"_v;
        parts.port_ = 443;
        scheme_end = 8;
    } else if (sv::starts_with(url, "http://"_v)) {
        parts.scheme_ = "http"_v;
        parts.port_ = 80;
        scheme_end = 7;
    } else {
        // No scheme -- assume http://
        parts.scheme_ = "http"_v;
        parts.port_ = 80;
        scheme_end = 0;
    }

    String_View rest = url.sub(scheme_end, url.length());

    u64 host_end = 0, port_start = 0;
    for (u64 i = 0; i < rest.length(); i++) {
        if (static_cast<char>(rest[i]) == ':') { host_end = i; port_start = i + 1; }
        if (static_cast<char>(rest[i]) == '/') { if (host_end == 0) host_end = i; break; }
    }
    if (host_end == 0) host_end = rest.length();
    parts.host_ = rest.sub(0, host_end);

    if (port_start > 0) {
        u64 pe = port_start;
        while (pe < rest.length() && rest[pe] >= '0' && rest[pe] <= '9') pe++;
        parts.port_ = 0;
        for (u64 i = port_start; i < pe; i++)
            parts.port_ = static_cast<u16>(parts.port_ * 10 + (rest[i] - '0'));
    }

    u64 path_start = 0;
    for (u64 i = 0; i < rest.length(); i++) {
        if (static_cast<char>(rest[i]) == '/') { path_start = i; break; }
    }
    parts.path_ = (path_start > 0) ? rest.sub(path_start, rest.length()) : "/"_v;

    return parts;
}

// Create a TCP socket and connect to host:port
[[nodiscard]] inline u64 http_tcp_connect(String_View host, u16 port, u64 timeout_ms = 10000) noexcept {
#ifdef SPP_OS_LINUX
    i32 fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return ~u64{0};

    i32 flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    String<> host_s{host.length() + 1};
    host_s.set_length(host.length() + 1);
    for (u64 i = 0; i < host.length(); i++) host_s[i] = host[i];
    host_s[host.length()] = '\0';

    char port_buf[8];
    (void)Libc::snprintf(reinterpret_cast<u8*>(port_buf), 8, "%u", port);

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = null;
    i32 ret = ::getaddrinfo(reinterpret_cast<const char*>(host_s.data()),
                            port_buf, &hints, &result);
    if (ret != 0 || !result) { ::close(fd); return ~u64{0}; }

    i32 flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    i32 conn_ret = ::connect(fd, result->ai_addr, static_cast<socklen_t>(result->ai_addrlen));
    ::freeaddrinfo(result);

    if (conn_ret < 0 && errno == EINPROGRESS) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        if (::select(fd + 1, null, &wfds, null, &tv) <= 0) { ::close(fd); return ~u64{0}; }
    }
    ::fcntl(fd, F_SETFL, flags);
    return static_cast<u64>(fd);
#else
    (void)host; (void)port; (void)timeout_ms;
    return ~u64{0};
#endif
}

// Close a TCP socket
inline void http_tcp_close(u64 fd) noexcept {
#ifdef SPP_OS_LINUX
    if (fd != ~u64{0}) ::close(static_cast<i32>(fd));
#else
    (void)fd;
#endif
}

// Send data over a TCP socket
[[nodiscard]] inline i64 http_tcp_send(u64 fd, const u8* data, u64 size) noexcept {
#ifdef SPP_OS_LINUX
    auto n = ::send(static_cast<i32>(fd), data, size, MSG_NOSIGNAL);
    return n >= 0 ? n : -1;
#else
    (void)fd; (void)data; (void)size;
    return -1;
#endif
}

// Receive data from a TCP socket with timeout
[[nodiscard]] inline i64 http_tcp_recv(u64 fd, u8* buffer, u64 size, u64 timeout_ms) noexcept {
#ifdef SPP_OS_LINUX
    if (timeout_ms > 0) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(static_cast<i32>(fd), &rfds);
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        i32 ret = ::select(static_cast<i32>(fd) + 1, &rfds, null, null, &tv);
        if (ret <= 0) return ret;
    }
    auto n = ::recv(static_cast<i32>(fd), buffer, size, 0);
    return n >= 0 ? n : -1;
#else
    (void)fd; (void)buffer; (void)size; (void)timeout_ms;
    return -1;
#endif
}

// TLS context for HTTPS connections
struct HTTPTLSContext {
#ifdef SPP_USE_OPENSSL
    void* ssl_ctx_ = null;  // SSL_CTX*
#endif
    bool initialized_ = false;

    bool init() noexcept {
#ifdef SPP_USE_OPENSSL
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) return false;
        SSL_CTX_set_min_proto_version(static_cast<SSL_CTX*>(ssl_ctx_), TLS1_2_VERSION);
        SSL_CTX_set_verify(static_cast<SSL_CTX*>(ssl_ctx_), SSL_VERIFY_PEER, null);
        SSL_CTX_set_default_verify_paths(static_cast<SSL_CTX*>(ssl_ctx_));
        initialized_ = true;
        return true;
#else
        return false;
#endif
    }

    void destroy() noexcept {
#ifdef SPP_USE_OPENSSL
        if (ssl_ctx_) { SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_)); ssl_ctx_ = null; }
#endif
        initialized_ = false;
    }
};

// Upgrade a TCP socket to TLS (HTTPS)
[[nodiscard]] inline bool http_tls_upgrade(u64 fd, String_View hostname,
                                            HTTPTLSContext& ctx, void*& out_ssl) noexcept {
#ifdef SPP_USE_OPENSSL
    if (!ctx.ssl_ctx_) return false;

    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx.ssl_ctx_));
    if (!ssl) return false;

    SSL_set_fd(ssl, static_cast<i32>(fd));

    String<> host_s{hostname.length() + 1};
    host_s.set_length(hostname.length() + 1);
    for (u64 i = 0; i < hostname.length(); i++) host_s[i] = hostname[i];
    host_s[hostname.length()] = '\0';
    SSL_set_tlsext_host_name(ssl, reinterpret_cast<const char*>(host_s.data()));

    i32 ret = SSL_connect(ssl);
    if (ret != 1) { SSL_free(ssl); return false; }

    out_ssl = ssl;
    return true;
#else
    (void)fd; (void)hostname; (void)ctx; (void)out_ssl;
    return false;
#endif
}

// Send data over TLS socket
[[nodiscard]] inline i64 http_tls_send(void* ssl, const u8* data, u64 size) noexcept {
#ifdef SPP_USE_OPENSSL
    i32 ret = SSL_write(static_cast<SSL*>(ssl), data, static_cast<i32>(size));
    return ret > 0 ? static_cast<i64>(ret) : -1;
#else
    (void)ssl; (void)data; (void)size;
    return -1;
#endif
}

// Receive data from TLS socket with timeout
[[nodiscard]] inline i64 http_tls_recv(u64 fd, void* ssl, u8* buffer, u64 size,
                                        u64 timeout_ms) noexcept {
#ifdef SPP_USE_OPENSSL
    // Check OpenSSL internal buffer first
    i32 pending = SSL_pending(static_cast<SSL*>(ssl));
    if (pending > 0) {
        i32 to_read = pending < static_cast<i32>(size) ? pending : static_cast<i32>(size);
        i32 ret = SSL_read(static_cast<SSL*>(ssl), buffer, to_read);
        return ret > 0 ? static_cast<i64>(ret) : -1;
    }

    // Poll underlying fd
    if (timeout_ms > 0 && fd != ~u64{0}) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(static_cast<i32>(fd), &rfds);
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        i32 ret = ::select(static_cast<i32>(fd) + 1, &rfds, null, null, &tv);
        if (ret <= 0) return ret;
    }

    i32 ret = SSL_read(static_cast<SSL*>(ssl), buffer, static_cast<i32>(size));
    if (ret <= 0) return -1;
    return static_cast<i64>(ret);
#else
    (void)fd; (void)ssl; (void)buffer; (void)size; (void)timeout_ms;
    return -1;
#endif
}

// Close TLS connection (does NOT close underlying TCP socket)
inline void http_tls_close(void* ssl) noexcept {
#ifdef SPP_USE_OPENSSL
    if (ssl) { SSL_shutdown(static_cast<SSL*>(ssl)); SSL_free(static_cast<SSL*>(ssl)); }
#else
    (void)ssl;
#endif
}

// -----------------------------------------------------------------------
// Build HTTP/1.1 request string from components
// -----------------------------------------------------------------------
// Returns the full request bytes ready to send over the wire

struct HTTPRequest {
    String_View method_;
    String_View path_;
    Vec<Pair<String<>, String<>>> headers_;  // owning strings for headers
    Vec<u8> body_;                            // request body (for POST/DELETE)
};

inline String<> build_http_request(const HTTPRequest& req, String_View host) noexcept {
    // Estimate size
    u64 est = req.method_.length() + req.path_.length() + 64;
    for (u64 i = 0; i < req.headers_.length(); i++)
        est += req.headers_[i].first.length() + req.headers_[i].second.length() + 4;
    est += req.body_.length();

    Vec<u8> buf;
    buf.reserve(est + 256);

    auto push_sv = [&buf](String_View s) {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    };

    push_sv(req.method_);
    buf.push(' ');
    push_sv(req.path_);
    push_sv(" HTTP/1.1\r\n"_v);
    push_sv("Host: "_v);
    push_sv(host);
    push_sv("\r\n"_v);

    for (u64 i = 0; i < req.headers_.length(); i++) {
        push_sv(req.headers_[i].first.view());
        push_sv(": "_v);
        push_sv(req.headers_[i].second.view());
        push_sv("\r\n"_v);
    }

    push_sv("\r\n"_v);

    if (!req.body_.empty()) {
        for (u64 i = 0; i < req.body_.length(); i++) buf.push(req.body_[i]);
    }

    String<> result{buf.length()};
    result.set_length(buf.length());
    for (u64 i = 0; i < buf.length(); i++) result[i] = buf[i];
    return result;
}

// -----------------------------------------------------------------------
// HTTP response parsing
// -----------------------------------------------------------------------

struct HTTPResponse {
    u64 status_code_ = 0;
    Vec<u8> body_;                   // raw response body
    Map<String<>, String<>> headers_;  // header name -> value

    // Get a header value (case-insensitive lookup)
    [[nodiscard]] Opt<String_View> header(String_View name) const noexcept {
        // Linear scan with case-insensitive comparison
        for (const auto& kv : headers_) {
            String_View k = kv.first.view();
            if (k.length() != name.length()) continue;
            bool match = true;
            for (u64 i = 0; i < k.length(); i++) {
                char a = static_cast<char>(k[i]);
                char b = static_cast<char>(name[i]);
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { match = false; break; }
            }
            if (match) return Opt<String_View>{kv.second.view()};
        }
        return {};
    }
};

// Parse HTTP response from raw bytes read from socket
// Returns the response and the number of bytes consumed from the buffer
inline Opt<HTTPResponse> parse_http_response(Slice<const u8> raw) noexcept {
    if (raw.length() < 16) return {};

    HTTPResponse resp;

    // ---- Parse status line ----
    u64 pos = 0;
    // Skip "HTTP/1.1 "
    if (raw[0] == 'H' && raw[1] == 'T' && raw[2] == 'T' && raw[3] == 'P' &&
        raw[4] == '/' && raw[5] != ' ') {
        pos = 7; // skip "HTTP/x."
        while (pos < raw.length() && raw[pos] != ' ') pos++;
        pos++; // skip space
    }
    // Read status code
    if (pos + 3 > raw.length()) return {};
    resp.status_code_ = 0;
    for (u64 i = 0; i < 3 && pos < raw.length() && raw[pos] >= '0' && raw[pos] <= '9'; i++) {
        resp.status_code_ = resp.status_code_ * 10 + static_cast<u64>(raw[pos] - '0');
        pos++;
    }

    // Skip to end of status line
    while (pos + 1 < raw.length() && !(raw[pos] == '\r' && raw[pos+1] == '\n')) pos++;
    pos += 2; // skip \r\n

    // ---- Parse headers ----
    while (pos + 1 < raw.length() && !(raw[pos] == '\r' && raw[pos+1] == '\n')) {
        u64 line_start = pos;
        while (pos + 1 < raw.length() && !(raw[pos] == '\r' && raw[pos+1] == '\n')) pos++;
        u64 line_end = pos;
        pos += 2; // skip \r\n

        if (line_end <= line_start) continue;

        // Find ':' separator
        u64 sep = line_start;
        while (sep < line_end && raw[sep] != ':') sep++;
        if (sep >= line_end) continue;

        // Extract header name (lowercased for case-insensitive storage)
        // Actually store as-is; lookup handles case
        String_View name_sv{raw.data() + line_start, sep - line_start};
        String<> name{name_sv.length()};
        name.set_length(name_sv.length());
        for (u64 i = 0; i < name_sv.length(); i++) name[i] = name_sv[i];

        // Skip ': ' after separator
        u64 val_start = sep + 1;
        while (val_start < line_end && raw[val_start] == ' ') val_start++;
        String_View val_sv{raw.data() + val_start, line_end - val_start};
        String<> value{val_sv.length()};
        value.set_length(val_sv.length());
        for (u64 i = 0; i < val_sv.length(); i++) value[i] = val_sv[i];

        resp.headers_.insert(spp::move(name), spp::move(value));
    }
    pos += 2; // skip empty line \r\n\r\n separator

    // ---- Read body based on Content-Length ----
    auto cl_opt = resp.header("Content-Length"_v);
    u64 content_length = 0;
    if (cl_opt.ok()) {
        String_View cl_sv = *cl_opt;
        for (u64 i = 0; i < cl_sv.length() && cl_sv[i] >= '0' && cl_sv[i] <= '9'; i++)
            content_length = content_length * 10 + static_cast<u64>(cl_sv[i] - '0');
    }

    if (content_length > 0) {
        if (pos + content_length > raw.length()) {
            // Not enough data yet -- partial response
            return {};
        }
        resp.body_.reserve(content_length);
        for (u64 i = 0; i < content_length; i++)
            resp.body_.push(raw[pos + i]);
    } else {
        // No Content-Length -- check for chunked transfer encoding
        auto te_opt = resp.header("Transfer-Encoding"_v);
        if (te_opt.ok()) {
            // [UNSPECIFIED] Chunked transfer encoding parsing not implemented.
            // For Binance REST, all responses use Content-Length.
            // If you need chunked support, implement RFC 7230 Section 4.1 here.
        }
        // Read whatever remains as body
        if (pos < raw.length()) {
            u64 remaining = raw.length() - pos;
            resp.body_.reserve(remaining);
            for (u64 i = 0; i < remaining; i++)
                resp.body_.push(raw[pos + i]);
        }
    }

    return Opt<HTTPResponse>{spp::move(resp)};
}

// -----------------------------------------------------------------------
// Full HTTP request/response cycle over a connection (TCP or TLS)
// -----------------------------------------------------------------------
struct HTTPConnection {
    u64 fd_ = ~u64{0};
    void* ssl_ = null;    // non-null if TLS
    bool use_tls_ = false;
    Vec<u8> recv_buf_;

    HTTPConnection() { recv_buf_.reserve(65536); }

    bool connect(String_View host, u16 port, bool use_tls, HTTPTLSContext& tls_ctx) noexcept {
        fd_ = http_tcp_connect(host, port);
        if (fd_ == ~u64{0}) return false;

        if (use_tls) {
#ifdef SPP_USE_OPENSSL
            if (!http_tls_upgrade(fd_, host, tls_ctx, ssl_)) {
                http_tcp_close(fd_);
                fd_ = ~u64{0};
                return false;
            }
            use_tls_ = true;
#else
            http_tcp_close(fd_);
            fd_ = ~u64{0};
            return false;
#endif
        }
        return true;
    }

    Opt<HTTPResponse> request(const HTTPRequest& req, String_View host, u64 timeout_ms = 30000) noexcept {
        if (fd_ == ~u64{0}) return {};

        String<> wire = build_http_request(req, host);

        // Send
        i64 sent;
        if (use_tls_) {
            sent = http_tls_send(ssl_, wire.data(), wire.length());
        } else {
            sent = http_tcp_send(fd_, wire.data(), wire.length());
        }
        if (sent != static_cast<i64>(wire.length())) return {};

        // Receive
        recv_buf_.clear();
        u8 tmp[8192];
        u64 total_read = 0;

        // Read headers first
        for (u64 attempt = 0; attempt < 20; attempt++) {
            u64 space = sizeof(tmp) - total_read;
            if (space == 0) break;
            i64 r;
            if (use_tls_) {
                r = http_tls_recv(fd_, ssl_, tmp + total_read, space,
                                  attempt == 0 ? timeout_ms : 2000);
            } else {
                r = http_tcp_recv(fd_, tmp + total_read, space,
                                  attempt == 0 ? timeout_ms : 2000);
            }
            if (r <= 0) break;
            total_read += static_cast<u64>(r);

            // Check for end of headers
            bool headers_done = false;
            for (u64 i = 0; i + 3 < total_read; i++) {
                if (tmp[i] == '\r' && tmp[i+1] == '\n' &&
                    tmp[i+2] == '\r' && tmp[i+3] == '\n') {
                    headers_done = true;
                    break;
                }
            }
            if (headers_done) {
                // Check for Content-Length to know if we need more body data
                auto partial = parse_http_response(
                    Slice<const u8>{tmp, total_read});
                if (partial.ok()) {
                    // Check if we have full body
                    auto cl = partial->header("Content-Length"_v);
                    u64 expected = 0;
                    if (cl.ok()) {
                        String_View clv = *cl;
                        for (u64 i = 0; i < clv.length() && clv[i] >= '0' && clv[i] <= '9'; i++)
                            expected = expected * 10 + static_cast<u64>(clv[i] - '0');
                    }
                    // Find body start
                    u64 body_start = 0;
                    for (u64 i = 0; i + 3 < total_read; i++) {
                        if (tmp[i] == '\r' && tmp[i+1] == '\n' &&
                            tmp[i+2] == '\r' && tmp[i+3] == '\n') {
                            body_start = i + 4;
                            break;
                        }
                    }
                    u64 body_got = total_read - body_start;
                    if (body_got >= expected) break;
                }
            }
        }

        // Build final Slice and parse
        for (u64 i = 0; i < total_read; i++) recv_buf_.push(tmp[i]);
        return parse_http_response(Slice<const u8>{recv_buf_.data(), recv_buf_.length()});
    }

    void disconnect() noexcept {
        if (use_tls_ && ssl_) http_tls_close(ssl_);
        http_tcp_close(fd_);
        fd_ = ~u64{0};
        ssl_ = null;
        use_tls_ = false;
    }
};

} // namespace http_detail

// =========================================================================
// AccountInfo -- standardized account snapshot
// =========================================================================
struct AccountInfo {
    f64 total_equity_ = 0.0;
    f64 available_balance_ = 0.0;
    f64 margin_used_ = 0.0;
    f64 margin_ratio_ = 0.0;
    f64 unrealized_pnl_ = 0.0;
    f64 realized_pnl_today_ = 0.0;
    Currency_Code base_currency_ = Currency_Code::USD;

    SPP_RECORD(AccountInfo, SPP_FIELD(total_equity_), SPP_FIELD(available_balance_),
               SPP_FIELD(margin_used_), SPP_FIELD(margin_ratio_),
               SPP_FIELD(unrealized_pnl_), SPP_FIELD(realized_pnl_today_));
};

// =========================================================================
// ExchangePosition -- position info reported by exchange
// =========================================================================
struct ExchangePosition {
    String<> symbol_;
    f64 quantity_ = 0.0;
    f64 avg_entry_price_ = 0.0;
    f64 mark_price_ = 0.0;
    f64 unrealized_pnl_ = 0.0;
    f64 liquidation_price_ = 0.0;
    f64 leverage_ = 1.0;
    Currency_Code currency_ = Currency_Code::USD;

    SPP_RECORD(ExchangePosition, SPP_FIELD(symbol_), SPP_FIELD(quantity_),
               SPP_FIELD(avg_entry_price_), SPP_FIELD(mark_price_),
               SPP_FIELD(unrealized_pnl_), SPP_FIELD(liquidation_price_),
               SPP_FIELD(leverage_));
};

// =========================================================================
// RESTResponse -- generic REST API response wrapper
// =========================================================================
template<typename T>
struct RESTResponse {
    bool success_ = false;
    u64 http_code_ = 0;
    String<> error_message_;
    Opt<T> data_;

    RESTResponse() = default;

    static RESTResponse ok(T&& value, u64 http_code = 200) noexcept {
        RESTResponse r;
        r.success_ = true;
        r.http_code_ = http_code;
        r.data_ = Opt<T>{spp::move(value)};
        return r;
    }

    static RESTResponse err(String_View msg, u64 http_code = 0) noexcept {
        RESTResponse r;
        r.success_ = false;
        r.http_code_ = http_code;
        r.error_message_ = sv::sv_to_string(msg);
        return r;
    }

    [[nodiscard]] bool ok() const noexcept { return success_ && data_.ok(); }
};

// =========================================================================
// RESTGateway -- abstract REST API interface for exchanges
// =========================================================================
struct RESTGateway {
    String_View name_;

    virtual ~RESTGateway() = default;

    virtual bool connect(String_View api_key, String_View api_secret) = 0;
    virtual bool is_connected() const = 0;

    virtual RESTResponse<AccountInfo> account_info() = 0;
    virtual RESTResponse<Vec<ExchangePosition>> positions() = 0;

    virtual RESTResponse<String<>> place_order(
        String_View symbol, OrderSide side, OrderType type,
        f64 quantity, f64 price = 0.0, f64 stop_price = 0.0) = 0;

    virtual RESTResponse<bool> cancel_order(String_View order_id, String_View symbol) = 0;
    virtual RESTResponse<bool> cancel_all_orders(String_View symbol = ""_v) = 0;

    virtual RESTResponse<Order> get_order(String_View order_id, String_View symbol) = 0;
    virtual RESTResponse<Vec<Order>> open_orders(String_View symbol = ""_v) = 0;

    virtual RESTResponse<Bar> latest_bar(String_View symbol, String_View interval = "1m"_v) = 0;
    virtual RESTResponse<QuoteTick> latest_quote(String_View symbol) = 0;
    virtual RESTResponse<Vec<Bar>> historical_bars(
        String_View symbol, String_View interval, Date from, Date to) = 0;

    virtual u64 rate_limit_remaining() const = 0;
    virtual u64 rate_limit_total() const = 0;
};

// =========================================================================
// BinanceREST -- Binance REST API implementation (HMAC-SHA256 auth)
// =========================================================================

struct BinanceREST : RESTGateway {
    String<> api_key_;
    String<> api_secret_;
    String<> base_url_;
    bool connected_ = false;

    http_detail::HTTPTLSContext tls_ctx_;
    http_detail::HTTPConnection conn_;

    // Rate limit tracking
    u64 rate_limit_remaining_ = 1200;
    u64 rate_limit_total_ = 1200;

    BinanceREST() noexcept {
        name_ = "BinanceREST"_v;
        base_url_ = sv::sv_to_string("https://api.binance.com"_v);
    }

    explicit BinanceREST(String_View base_url) noexcept {
        name_ = "BinanceREST"_v;
        base_url_ = sv::sv_to_string(base_url);
    }

    ~BinanceREST() override { disconnect(); }

    // -----------------------------------------------------------------
    // Connection
    // -----------------------------------------------------------------
    bool connect(String_View api_key, String_View api_secret) override {
        api_key_ = sv::sv_to_string(api_key);
        api_secret_ = sv::sv_to_string(api_secret);

        if (sv::starts_with(base_url_.view(), "https://"_v)) {
            if (!tls_ctx_.init()) return false;
        }

        auto parts = http_detail::parse_url(base_url_.view());
        bool use_tls = (parts.scheme_ == "https"_v);

        if (!conn_.connect(parts.host_, parts.port_, use_tls, tls_ctx_)) {
            if (use_tls) tls_ctx_.destroy();
            return false;
        }
        connected_ = true;
        return true;
    }

    bool is_connected() const override { return connected_; }

    void disconnect() {
        conn_.disconnect();
        tls_ctx_.destroy();
        connected_ = false;
    }

    // -----------------------------------------------------------------
    // Account
    // -----------------------------------------------------------------
    RESTResponse<AccountInfo> account_info() override {
        auto resp = http_get("/api/v3/account"_v, ""_v);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<AccountInfo>::err("Failed to fetch account info"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        AccountInfo info;

        // Parse Binance account response
        auto balances_start = "balances"_v;
        info.total_equity_ = 0.0;

        auto can_trade = json_detail::get_bool_value(body, "canTrade"_v);

        // Calculate total equity from balances
        u64 pos = 0;
        while (pos < body.length()) {
            // Find "asset"
            for (; pos + 7 < body.length(); pos++) {
                if (static_cast<char>(body[pos]) == '"' &&
                    body[pos+1] == 'a' && body[pos+2] == 's' &&
                    body[pos+3] == 's' && body[pos+4] == 'e' &&
                    body[pos+5] == 't' && body[pos+6] == '"') {
                    pos += 7;
                    while (pos < body.length() && body[pos] != ':') pos++;
                    pos++; // skip ':'
                    while (pos < body.length() && body[pos] != '"') pos++;
                    // simplified: use get_f64_value for "free" and "locked"
                    break;
                }
            }
            break;
        }

        // Simplified: fetch key fields from account
        auto total_margin = json_detail::get_f64_value(body, "totalMarginBalance"_v);
        auto available = json_detail::get_f64_value(body, "availableBalance"_v);
        auto unrealized = json_detail::get_f64_value(body, "totalUnrealizedProfit"_v);
        auto total_wallet = json_detail::get_f64_value(body, "totalWalletBalance"_v);

        if (total_wallet.ok()) info.total_equity_ = *total_wallet;
        else if (total_margin.ok()) info.total_equity_ = *total_margin;
        if (available.ok()) info.available_balance_ = *available;
        if (unrealized.ok()) info.unrealized_pnl_ = *unrealized;
        if (total_margin.ok() && total_wallet.ok() && *total_wallet > 0.0) {
            f64 used = *total_margin - *total_wallet;
            info.margin_used_ = used > 0.0 ? used : 0.0;
            info.margin_ratio_ = info.margin_used_ / info.total_equity_;
        }

        info.base_currency_ = Currency_Code::USD;
        return RESTResponse<AccountInfo>::ok(spp::move(info));
    }

    // -----------------------------------------------------------------
    // Positions
    // -----------------------------------------------------------------
    RESTResponse<Vec<ExchangePosition>> positions() override {
        auto resp = http_get("/api/v3/account"_v, ""_v);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<Vec<ExchangePosition>>::err("Failed to fetch positions"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        Vec<ExchangePosition> result;

        // Parse "positions" array from Binance response
        // Simplified: look for position entries
        // In Binance response, positions are in "positions":[{"symbol":"BTCUSDT",...}]
        //
        // We do a simple scan for position objects and extract key fields
        u64 pos = 0;
        while (pos + 10 < body.length()) {
            // Find "symbol" key
            if (body[pos] == '"' && body[pos+1] == 's' && body[pos+2] == 'y' &&
                body[pos+3] == 'm' && body[pos+4] == 'b' && body[pos+5] == 'o' &&
                body[pos+6] == 'l' && body[pos+7] == '"') {
                // Found a position entry
                u64 obj_start = pos;
                while (obj_start > 0 && body[obj_start] != '{') obj_start--;
                u64 obj_end = pos;
                i32 depth = 0;
                for (; obj_end < body.length(); obj_end++) {
                    if (body[obj_end] == '{') depth++;
                    else if (body[obj_end] == '}') { depth--; if (depth == 0) break; }
                }
                if (obj_end < body.length()) {
                    String_View pos_obj = body.sub(obj_start, obj_end + 1);
                    auto sym = json_detail::get_string_value(pos_obj, "symbol"_v);
                    auto qty = json_detail::get_f64_value(pos_obj, "positionAmt"_v);
                    auto entry = json_detail::get_f64_value(pos_obj, "entryPrice"_v);
                    auto mark = json_detail::get_f64_value(pos_obj, "markPrice"_v);
                    auto upnl = json_detail::get_f64_value(pos_obj, "unRealizedProfit"_v);
                    auto liq = json_detail::get_f64_value(pos_obj, "liquidationPrice"_v);
                    auto lev = json_detail::get_f64_value(pos_obj, "leverage"_v);

                    if (sym.ok() && qty.ok() && *qty != 0.0) {
                        ExchangePosition ep;
                        ep.symbol_ = sv::sv_to_string(*sym);
                        ep.quantity_ = *qty;
                        if (entry.ok()) ep.avg_entry_price_ = *entry;
                        if (mark.ok()) ep.mark_price_ = *mark;
                        if (upnl.ok()) ep.unrealized_pnl_ = *upnl;
                        if (liq.ok()) ep.liquidation_price_ = *liq;
                        if (lev.ok()) ep.leverage_ = *lev;
                        ep.currency_ = Currency_Code::USD;
                        result.push(spp::move(ep));
                    }
                }
                pos = obj_end + 1;
                continue;
            }
            pos++;
        }

        return RESTResponse<Vec<ExchangePosition>>::ok(spp::move(result));
    }

    // -----------------------------------------------------------------
    // Place order
    // -----------------------------------------------------------------
    RESTResponse<String<>> place_order(
        String_View symbol, OrderSide side, OrderType type,
        f64 quantity, f64 price = 0.0, f64 stop_price = 0.0) override {

        // Build query string
        Vec<u8> qs;
        qs.reserve(512);
        auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };

        push_sv("symbol="_v); push_sv(symbol);
        push_sv("&side="_v);
        push_sv(side == OrderSide::Buy ? "BUY"_v : "SELL"_v);

        // Order type
        switch (type) {
        case OrderType::Market: push_sv("&type=MARKET"_v); break;
        case OrderType::Limit:  push_sv("&type=LIMIT"_v); break;
        case OrderType::Stop:   push_sv("&type=STOP_LOSS"_v); break;
        case OrderType::StopLimit: push_sv("&type=STOP_LOSS_LIMIT"_v); break;
        default:                push_sv("&type=MARKET"_v); break;
        }

        // Quantity
        char qbuf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(qbuf), 32, "%.8f", quantity);
        push_sv("&quantity="_v);
        for (u64 i = 0; i < 32 && qbuf[i] != '\0'; i++) qs.push(static_cast<u8>(qbuf[i]));

        // Price (for limit orders)
        if (price > 0.0 && type != OrderType::Market) {
            char pbuf[32];
            (void)Libc::snprintf(reinterpret_cast<u8*>(pbuf), 32, "%.2f", price);
            push_sv("&price="_v);
            for (u64 i = 0; i < 32 && pbuf[i] != '\0'; i++) qs.push(static_cast<u8>(pbuf[i]));
        }

        // Stop price
        if (stop_price > 0.0) {
            char sbuf[32];
            (void)Libc::snprintf(reinterpret_cast<u8*>(sbuf), 32, "%.2f", stop_price);
            push_sv("&stopPrice="_v);
            for (u64 i = 0; i < 32 && sbuf[i] != '\0'; i++) qs.push(static_cast<u8>(sbuf[i]));
        }

        // Time in force for limit orders
        if (type != OrderType::Market) {
            push_sv("&timeInForce=GTC"_v);
        }

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_post("/api/v3/order"_v, query_str);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<String<>>::err("Order placement failed"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        auto order_id = json_detail::get_string_value(body, "orderId"_v);
        auto client_id = json_detail::get_string_value(body, "clientOrderId"_v);

        if (order_id.ok()) {
            return RESTResponse<String<>>::ok(sv::sv_to_string(*order_id));
        }
        if (client_id.ok()) {
            return RESTResponse<String<>>::ok(sv::sv_to_string(*client_id));
        }
        return RESTResponse<String<>>::err("No order ID in response"_v);
    }

    // -----------------------------------------------------------------
    // Cancel order
    // -----------------------------------------------------------------
    RESTResponse<bool> cancel_order(String_View order_id, String_View symbol) override {
        Vec<u8> qs;
        qs.reserve(128);
        auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };
        push_sv("symbol="_v); push_sv(symbol);
        push_sv("&orderId="_v); push_sv(order_id);

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_delete("/api/v3/order"_v, query_str);
        if (!resp.ok()) {
            return RESTResponse<bool>::err("Cancel failed"_v, 0);
        }
        return RESTResponse<bool>::ok(true, resp->status_code_);
    }

    // -----------------------------------------------------------------
    // Cancel all orders
    // -----------------------------------------------------------------
    RESTResponse<bool> cancel_all_orders(String_View symbol) override {
        Vec<u8> qs;
        qs.reserve(128);
        if (!symbol.empty()) {
            auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };
            push_sv("symbol="_v); push_sv(symbol);
        }

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_delete("/api/v3/openOrders"_v, query_str);
        if (!resp.ok()) {
            return RESTResponse<bool>::err("Cancel all failed"_v, 0);
        }
        return RESTResponse<bool>::ok(true, resp->status_code_);
    }

    // -----------------------------------------------------------------
    // Get order
    // -----------------------------------------------------------------
    RESTResponse<Order> get_order(String_View order_id, String_View symbol) override {
        Vec<u8> qs;
        qs.reserve(128);
        auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };
        push_sv("symbol="_v); push_sv(symbol);
        push_sv("&orderId="_v); push_sv(order_id);

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_get("/api/v3/order"_v, query_str);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<Order>::err("Get order failed"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        return parse_order_response(body);
    }

    // -----------------------------------------------------------------
    // Open orders
    // -----------------------------------------------------------------
    RESTResponse<Vec<Order>> open_orders(String_View symbol) override {
        Vec<u8> qs;
        qs.reserve(128);
        if (!symbol.empty()) {
            auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };
            push_sv("symbol="_v); push_sv(symbol);
        }

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_get("/api/v3/openOrders"_v, query_str);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<Vec<Order>>::err("Get open orders failed"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        Vec<Order> result;

        // Parse array of order objects
        u64 pos = 0;
        while (pos < body.length()) {
            if (body[pos] == '{') {
                u64 end = pos;
                i32 depth = 0;
                for (; end < body.length(); end++) {
                    if (body[end] == '{') depth++;
                    else if (body[end] == '}') { depth--; if (depth == 0) break; }
                }
                if (end < body.length()) {
                    auto order_or = parse_order_response(body.sub(pos, end + 1));
                    if (order_or.ok() && order_or.data_.ok()) {
                        result.push(spp::move(*order_or.data_));
                    }
                    pos = end + 1;
                    continue;
                }
            }
            pos++;
        }

        return RESTResponse<Vec<Order>>::ok(spp::move(result));
    }

    // -----------------------------------------------------------------
    // Market data via REST
    // -----------------------------------------------------------------
    RESTResponse<Bar> latest_bar(String_View symbol, String_View interval) override {
        Vec<u8> qs;
        qs.reserve(128);
        auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };
        push_sv("symbol="_v); push_sv(symbol);
        push_sv("&interval="_v); push_sv(interval);
        push_sv("&limit=1"_v);

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_get("/api/v3/klines"_v, query_str);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<Bar>::err("Failed to fetch klines"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        // Binance returns: [[time, open, high, low, close, volume, ...], ...]
        // Parse the first inner array
        return parse_kline_from_rest(body);
    }

    RESTResponse<QuoteTick> latest_quote(String_View symbol) override {
        Vec<u8> qs;
        qs.reserve(128);
        auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };
        push_sv("symbol="_v); push_sv(symbol);

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_get("/api/v3/ticker/bookTicker"_v, query_str);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<QuoteTick>::err("Failed to fetch quote"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        auto bid = json_detail::get_f64_value(body, "bidPrice"_v);
        auto ask = json_detail::get_f64_value(body, "askPrice"_v);
        auto bid_sz = json_detail::get_f64_value(body, "bidQty"_v);
        auto ask_sz = json_detail::get_f64_value(body, "askQty"_v);

        if (!bid.ok() || !ask.ok()) {
            return RESTResponse<QuoteTick>::err("Failed to parse quote"_v);
        }

        QuoteTick qt;
        qt.timestamp_ = Date::today();
        qt.bid_ = *bid;
        qt.ask_ = *ask;
        qt.bid_size_ = bid_sz.ok() ? *bid_sz : 0.0;
        qt.ask_size_ = ask_sz.ok() ? *ask_sz : 0.0;

        return RESTResponse<QuoteTick>::ok(spp::move(qt));
    }

    RESTResponse<Vec<Bar>> historical_bars(
        String_View symbol, String_View interval, Date from, Date to) override {

        Vec<u8> qs;
        qs.reserve(256);
        auto push_sv = [&qs](String_View s) { for (u64 i = 0; i < s.length(); i++) qs.push(s[i]); };

        // Convert dates to millisecond timestamps
        i64 from_ms = static_cast<i64>(from.serial_ - 25569) * 86400000LL;
        i64 to_ms = static_cast<i64>(to.serial_ - 25569) * 86400000LL;

        char tbuf[32];
        push_sv("symbol="_v); push_sv(symbol);
        push_sv("&interval="_v); push_sv(interval);
        (void)Libc::snprintf(reinterpret_cast<u8*>(tbuf), 32, "%lld", static_cast<long long>(from_ms));
        push_sv("&startTime="_v);
        for (u64 i = 0; i < 32 && tbuf[i] != '\0'; i++) qs.push(static_cast<u8>(tbuf[i]));
        (void)Libc::snprintf(reinterpret_cast<u8*>(tbuf), 32, "%lld", static_cast<long long>(to_ms));
        push_sv("&endTime="_v);
        for (u64 i = 0; i < 32 && tbuf[i] != '\0'; i++) qs.push(static_cast<u8>(tbuf[i]));
        push_sv("&limit=1000"_v);

        String_View query_str{qs.data(), qs.length()};
        auto resp = http_get("/api/v3/klines"_v, query_str);
        if (!resp.ok() || resp->status_code_ != 200) {
            return RESTResponse<Vec<Bar>>::err("Failed to fetch historical bars"_v, resp.ok() ? resp->status_code_ : 0);
        }

        String_View body{resp->body_.data(), resp->body_.length()};
        Vec<Bar> result;

        // Parse each kline entry: [[t,o,h,l,c,v,...], ...]
        u64 pos = 0;
        while (pos < body.length()) {
            if (body[pos] == '[') {
                u64 end = pos;
                i32 depth = 0;
                for (; end < body.length(); end++) {
                    if (body[end] == '[') depth++;
                    else if (body[end] == ']') { depth--; if (depth == 0) break; }
                }
                if (end < body.length()) {
                    auto bar_opt = parse_kline_array_entry(body.sub(pos, end + 1));
                    if (bar_opt.ok()) result.push(spp::move(*bar_opt));
                    pos = end + 1;
                    continue;
                }
            }
            pos++;
        }

        return RESTResponse<Vec<Bar>>::ok(spp::move(result));
    }

    // -----------------------------------------------------------------
    // Rate limits
    // -----------------------------------------------------------------
    u64 rate_limit_remaining() const override { return rate_limit_remaining_; }
    u64 rate_limit_total() const override { return rate_limit_total_; }

private:
    // =================================================================
    // Binance signature: HMAC-SHA256(query_string, api_secret) hex
    // =================================================================
    String<> sign(String_View query_string) const noexcept {
        return hmac_detail::hmac_sha256_hex(api_secret_.view(), query_string);
    }

    // =================================================================
    // HTTP methods with Binance auth headers
    // =================================================================

    Opt<http_detail::HTTPResponse> http_get(String_View path, String_View query) {
        return http_request("GET"_v, path, query, ""_v);
    }

    Opt<http_detail::HTTPResponse> http_post(String_View path, String_View query) {
        return http_request("POST"_v, path, query, query);
    }

    Opt<http_detail::HTTPResponse> http_delete(String_View path, String_View query) {
        return http_request("DELETE"_v, path, query, query);
    }

    Opt<http_detail::HTTPResponse> http_request(String_View method, String_View path,
                                                  String_View query, String_View body) {
        if (!connected_) return {};

        // Add timestamp and signature to query
        i64 ts = static_cast<i64>(Date::today().serial_ - 25569) * 86400000LL;
        // [UNSPECIFIED] Timestamp resolution is day-level, not millisecond.
        // In production, use std::chrono::system_clock for ms precision.

        Vec<u8> full_query_buf;
        full_query_buf.reserve(query.length() + 128);

        for (u64 i = 0; i < query.length(); i++) full_query_buf.push(query[i]);

        char ts_buf[24];
        (void)Libc::snprintf(reinterpret_cast<u8*>(ts_buf), 24, "&timestamp=%lld",
                             static_cast<long long>(ts));
        for (u64 i = 0; i < 24 && ts_buf[i] != '\0'; i++) full_query_buf.push(static_cast<u8>(ts_buf[i]));

        // Build query string for signing
        String_View qs_for_sig{full_query_buf.data(), full_query_buf.length()};
        String<> signature = sign(qs_for_sig);

        // Append signature to query
        auto push_sv = [&full_query_buf](String_View s) {
            for (u64 i = 0; i < s.length(); i++) full_query_buf.push(s[i]);
        };
        push_sv("&signature="_v);
        push_sv(signature.view());

        // Build full path
        Vec<u8> full_path_buf;
        full_path_buf.reserve(path.length() + full_query_buf.length() + 2);
        push_sv_into(full_path_buf, path);
        full_path_buf.push('?');
        for (u64 i = 0; i < full_query_buf.length(); i++)
            full_path_buf.push(full_query_buf[i]);

        // Build HTTP request
        http_detail::HTTPRequest req;
        req.method_ = method;
        req.path_ = String_View{full_path_buf.data(), full_path_buf.length()};

        // Headers
        req.headers_.push(Pair<String<>, String<>>{
            sv::sv_to_string("User-Agent"_v),
            sv::sv_to_string("spp-quant/1.0"_v)
        });
        req.headers_.push(Pair<String<>, String<>>{
            sv::sv_to_string("X-MBX-APIKEY"_v),
            String<>{api_key_}
        });

        auto parts = http_detail::parse_url(base_url_.view());

        if (method != "GET"_v) {
            req.headers_.push(Pair<String<>, String<>>{
                sv::sv_to_string("Content-Type"_v),
                sv::sv_to_string("application/x-www-form-urlencoded"_v)
            });
        }

        return conn_.request(req, parts.host_, 15000);
    }

    // Helper: push String_View into Vec<u8>
    static void push_sv_into(Vec<u8>& buf, String_View s) noexcept {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    }

    // =================================================================
    // Parse a single kline array entry: [t, o, h, l, c, v, ...]
    // =================================================================
    static Opt<Bar> parse_kline_array_entry(String_View json) noexcept {
        // Format: [1499040000000, "0.01634000", "0.01635000", ...]
        // Fields: 0=open_time, 1=open, 2=high, 3=low, 4=close, 5=volume,
        //         6=close_time, 7=quote_vol, 8=trades, ...
        if (json.empty() || json[0] != '[') return {};

        // Parse comma-separated values
        struct { f64 v; bool ok; } fields[9] = {};
        u64 pos = 1;
        for (i32 fi = 0; fi < 9 && pos < json.length(); fi++) {
            while (pos < json.length() && (json[pos] == ' ' || json[pos] == ',')) pos++;
            if (pos >= json.length() || json[pos] == ']') break;

            u64 start = pos;
            if (json[pos] == '"') { pos++; start = pos; while (pos < json.length() && json[pos] != '"') pos++; }
            else while (pos < json.length() && json[pos] != ',' && json[pos] != ']') pos++;

            String_View val = json.sub(start, pos);
            if (json[pos] == '"') pos++;

            f64 num = 0.0;
            bool neg = false;
            u64 vp = 0;
            if (!val.empty() && static_cast<char>(val[0]) == '-') { neg = true; vp++; }
            f64 frac = 0.1;
            bool in_frac = false;
            while (vp < val.length()) {
                char c = static_cast<char>(val[vp]);
                if (c >= '0' && c <= '9') {
                    if (in_frac) { num += static_cast<f64>(c - '0') * frac; frac *= 0.1; }
                    else num = num * 10.0 + static_cast<f64>(c - '0');
                } else if (c == '.') { in_frac = true; }
                vp++;
            }
            if (neg) num = -num;
            fields[fi].v = num;
            fields[fi].ok = pos > start;
        }

        if (!fields[1].ok || !fields[4].ok) return {};

        Bar bar;
        if (fields[0].ok) {
            i64 ts_ms = static_cast<i64>(fields[0].v);
            bar.date_ = Date{static_cast<i32>(ts_ms / 86400000 + 25569)};
        } else {
            bar.date_ = Date::today();
        }
        bar.open_ = fields[1].v;
        bar.high_ = fields[2].ok ? fields[2].v : bar.open_;
        bar.low_ = fields[3].ok ? fields[3].v : bar.open_;
        bar.close_ = fields[4].v;
        bar.volume_ = fields[5].ok ? fields[5].v : 0.0;
        bar.trades_count_ = fields[8].ok ? static_cast<u64>(fields[8].v) : 0;

        if (bar.volume_ > 0.0) {
            bar.vwap_ = (bar.high_ + bar.low_ + bar.close_) / 3.0;
        }

        return Opt<Bar>{spp::move(bar)};
    }

    // =================================================================
    // Parse REST kline response (wraps outer array)
    // =================================================================
    RESTResponse<Bar> parse_kline_from_rest(String_View body) noexcept {
        // Binance returns: [[t,o,h,l,c,v,...]]
        u64 pos = 0;
        while (pos < body.length() && body[pos] != '[') pos++;
        auto bar_opt = parse_kline_array_entry(body.sub(pos, body.length()));
        if (bar_opt.ok()) {
            return RESTResponse<Bar>::ok(spp::move(*bar_opt));
        }
        return RESTResponse<Bar>::err("Failed to parse kline"_v);
    }

    // =================================================================
    // Parse order response JSON into Order struct
    // =================================================================
    static RESTResponse<Order> parse_order_response(String_View json) noexcept {
        using json_detail::get_f64_value;
        using json_detail::get_string_value;

        auto order_id_str = get_string_value(json, "orderId"_v);
        auto side_str = get_string_value(json, "side"_v);
        auto status_str = get_string_value(json, "status"_v);
        auto type_str = get_string_value(json, "type"_v);
        auto qty = get_f64_value(json, "origQty"_v);
        auto filled = get_f64_value(json, "executedQty"_v);
        auto price = get_f64_value(json, "price"_v);
        auto stop_px = get_f64_value(json, "stopPrice"_v);
        auto avg_px = get_f64_value(json, "cummulativeQuoteQty"_v);

        if (!order_id_str.ok() || !side_str.ok() || !qty.ok()) {
            return RESTResponse<Order>::err("Incomplete order data"_v);
        }

        Order order;
        order.id_ = 0;
        // Parse order ID from string
        String_View oid_sv = *order_id_str;
        for (u64 i = 0; i < oid_sv.length(); i++) {
            if (oid_sv[i] >= '0' && oid_sv[i] <= '9')
                order.id_ = order.id_ * 10 + static_cast<u64>(oid_sv[i] - '0');
        }

        order.created_ = Date::today();
        order.last_update_ = Date::today();

        if (side_str.ok()) {
            order.side_ = (*side_str == "BUY"_v || *side_str == "Buy"_v)
                          ? OrderSide::Buy : OrderSide::Sell;
        }

        if (type_str.ok()) {
            String_View tv = *type_str;
            if (tv == "MARKET"_v || tv == "Market"_v) order.type_ = OrderType::Market;
            else if (tv == "LIMIT"_v || tv == "Limit"_v) order.type_ = OrderType::Limit;
            else if (tv == "STOP_LOSS"_v) order.type_ = OrderType::Stop;
            else if (tv == "STOP_LOSS_LIMIT"_v) order.type_ = OrderType::StopLimit;
            else order.type_ = OrderType::Market;
        }

        if (status_str.ok()) {
            String_View sv = *status_str;
            if (sv == "NEW"_v || sv == "New"_v) order.status_ = OrderStatus::New;
            else if (sv == "PARTIALLY_FILLED"_v || sv == "PartiallyFilled"_v)
                order.status_ = OrderStatus::Partial;
            else if (sv == "FILLED"_v || sv == "Filled"_v) order.status_ = OrderStatus::Filled;
            else if (sv == "CANCELED"_v || sv == "Canceled"_v || sv == "CANCELLED"_v)
                order.status_ = OrderStatus::Cancelled;
            else if (sv == "REJECTED"_v || sv == "Rejected"_v)
                order.status_ = OrderStatus::Rejected;
            else order.status_ = OrderStatus::New;
        }

        order.quantity_ = *qty;
        if (filled.ok()) order.filled_quantity_ = *filled;
        if (price.ok()) order.limit_price_ = *price;
        if (stop_px.ok()) order.stop_price_ = *stop_px;
        if (avg_px.ok() && order.filled_quantity_ > 0.0)
            order.avg_fill_price_ = *avg_px / order.filled_quantity_;

        return RESTResponse<Order>::ok(spp::move(order));
    }
};

} // namespace spp::quant::execution

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::execution::AccountInfo, "AccountInfo",
                 SPP_FIELD(total_equity_), SPP_FIELD(available_balance_),
                 SPP_FIELD(margin_used_), SPP_FIELD(margin_ratio_),
                 SPP_FIELD(unrealized_pnl_), SPP_FIELD(realized_pnl_today_));

SPP_NAMED_RECORD(::spp::quant::execution::ExchangePosition, "ExchangePosition",
                 SPP_FIELD(symbol_), SPP_FIELD(quantity_), SPP_FIELD(avg_entry_price_),
                 SPP_FIELD(mark_price_), SPP_FIELD(unrealized_pnl_),
                 SPP_FIELD(liquidation_price_), SPP_FIELD(leverage_));
