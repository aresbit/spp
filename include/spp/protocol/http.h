#pragma once

#include <spp/core/base.h>
#include <spp/io/net.h>
#include <spp/io/stream.h>

namespace spp::Protocol::Http {

// Minimal HTTP/1.1 client. Encodes a request to bytes (no buffered stream;
// caller hands the bytes to a Tcp_Client or TLS-wrapped stream) and parses a
// well-formed response with Content-Length-delimited body. chunked transfer
// encoding is not yet implemented; Binance Spot REST always uses
// Content-Length so this is sufficient for the immediate use case.

struct Header {
    String_View name;
    String_View value;
};

template<Allocator A = Mdefault>
struct Request {
    String_View method = "GET"_v;
    String_View path = "/"_v; // expected to already contain any query string
    String_View host;
    Vec<Header, A> headers;
    Slice<const u8> body;

    Request() noexcept = default;
    explicit Request(u64 hdr_cap) noexcept : headers(hdr_cap) {
    }

    template<Allocator B = A>
    [[nodiscard]] Vec<u8, B> to_bytes() const noexcept {
        // Pre-size to a generous estimate; Vec will grow if needed.
        u64 cap = method.length() + path.length() + host.length() + body.length() + 256;
        for(const auto& h : headers) cap += h.name.length() + h.value.length() + 4;

        Vec<u8, B> out(cap);
        auto put_sv = [&out](String_View sv) {
            for(u64 i = 0; i < sv.length(); i++) out.push(sv[i]);
        };
        auto put_lit = [&out](const char* s) {
            while(*s) out.push(static_cast<u8>(*s++));
        };

        put_sv(method);
        out.push(' ');
        put_sv(path);
        put_lit(" HTTP/1.1\r\n");

        if(host.length() > 0) {
            put_lit("Host: ");
            put_sv(host);
            put_lit("\r\n");
        }

        bool has_content_length = false;
        for(const auto& h : headers) {
            if(h.name.length() == 14) {
                bool match = true;
                static constexpr const char* lc = "content-length";
                for(u64 i = 0; i < 14; i++) {
                    u8 c = h.name[i];
                    if(c >= 'A' && c <= 'Z') c = c + 32;
                    if(c != static_cast<u8>(lc[i])) {
                        match = false;
                        break;
                    }
                }
                if(match) has_content_length = true;
            }
            put_sv(h.name);
            put_lit(": ");
            put_sv(h.value);
            put_lit("\r\n");
        }

        if(body.length() > 0 && !has_content_length) {
            u8 len_buf[32];
            i32 n = Libc::snprintf(len_buf, sizeof(len_buf), "%lu",
                                   static_cast<unsigned long>(body.length()));
            if(n > 0) {
                put_lit("Content-Length: ");
                for(i32 i = 0; i < n; i++) out.push(len_buf[i]);
                put_lit("\r\n");
            }
        }

        put_lit("\r\n");
        for(u64 i = 0; i < body.length(); i++) out.push(body[i]);
        return out;
    }
};

template<Allocator A = Mdefault>
struct Response {
    u32 status_code = 0;
    String_View status_text;
    Vec<Header, A> headers;
    Slice<const u8> body; // borrows from the parsed input buffer

    [[nodiscard]] Opt<String_View> find_header(String_View name) const noexcept {
        for(const auto& h : headers) {
            if(h.name.length() != name.length()) continue;
            bool match = true;
            for(u64 i = 0; i < name.length(); i++) {
                u8 a = h.name[i];
                u8 b = name[i];
                if(a >= 'A' && a <= 'Z') a = a + 32;
                if(b >= 'A' && b <= 'Z') b = b + 32;
                if(a != b) {
                    match = false;
                    break;
                }
            }
            if(match) return Opt<String_View>{h.value};
        }
        return {};
    }
};

namespace detail {

[[nodiscard]] inline bool starts_with(Slice<const u8> s, const char* prefix) noexcept {
    u64 i = 0;
    while(prefix[i]) {
        if(i >= s.length()) return false;
        if(s[i] != static_cast<u8>(prefix[i])) return false;
        i++;
    }
    return true;
}

[[nodiscard]] inline u64 find_crlf(Slice<const u8> s, u64 from) noexcept {
    for(u64 i = from; i + 1 < s.length(); i++) {
        if(s[i] == '\r' && s[i + 1] == '\n') return i;
    }
    return s.length();
}

[[nodiscard]] inline String_View trim_ascii_ws(String_View s) noexcept {
    u64 b = 0, e = s.length();
    while(b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.sub(b, e);
}

[[nodiscard]] inline Result<u64, String_View> parse_decimal_u64(String_View s) noexcept {
    if(s.empty()) return Result<u64, String_View>::err("http_bad_number"_v);
    u64 v = 0;
    for(u8 c : s) {
        if(c < '0' || c > '9') return Result<u64, String_View>::err("http_bad_number"_v);
        v = v * 10 + (c - '0');
    }
    return Result<u64, String_View>::ok(spp::move(v));
}

} // namespace detail

template<Allocator A = Mdefault>
[[nodiscard]] Result<Response<A>, String_View>
parse_response_headers(Slice<const u8> bytes, u64& headers_end) noexcept {
    using Err = Result<Response<A>, String_View>;
    if(!detail::starts_with(bytes, "HTTP/1.")) {
        return Err::err("http_bad_status_line"_v);
    }
    u64 status_end = detail::find_crlf(bytes, 0);
    if(status_end >= bytes.length()) return Err::err("http_status_unterminated"_v);

    // HTTP/1.x SP CODE SP TEXT
    if(bytes.length() < 12 || bytes[8] != ' ') return Err::err("http_bad_status_line"_v);
    u64 code_start = 9;
    u64 code_end = code_start;
    while(code_end < status_end && bytes[code_end] != ' ') code_end++;
    if(code_end == code_start) return Err::err("http_bad_status_code"_v);
    auto code = detail::parse_decimal_u64(
        String_View{bytes.data() + code_start, code_end - code_start});
    if(!code.ok()) return Err::err(spp::move(code.unwrap_err()));

    String_View status_text;
    if(code_end < status_end) {
        u64 text_start = code_end + 1;
        status_text = String_View{bytes.data() + text_start, status_end - text_start};
    }

    Response<A> resp;
    resp.status_code = static_cast<u32>(code.unwrap());
    resp.status_text = status_text;

    u64 pos = status_end + 2;
    for(;;) {
        if(pos + 1 >= bytes.length()) return Err::err("http_headers_unterminated"_v);
        if(bytes[pos] == '\r' && bytes[pos + 1] == '\n') {
            pos += 2;
            break;
        }
        u64 line_end = detail::find_crlf(bytes, pos);
        if(line_end >= bytes.length()) return Err::err("http_header_unterminated"_v);

        u64 colon = pos;
        while(colon < line_end && bytes[colon] != ':') colon++;
        if(colon == line_end) return Err::err("http_header_no_colon"_v);

        Header h;
        h.name = String_View{bytes.data() + pos, colon - pos};
        h.value = detail::trim_ascii_ws(
            String_View{bytes.data() + colon + 1, line_end - colon - 1});
        resp.headers.push(spp::move(h));
        pos = line_end + 2;
    }

    headers_end = pos;
    resp.body = Slice<const u8>{bytes.data() + pos, 0};
    return Err::ok(spp::move(resp));
}

template<Allocator A = Mdefault>
[[nodiscard]] Result<Response<A>, String_View>
parse_response(Slice<const u8> bytes) noexcept {
    using Err = Result<Response<A>, String_View>;
    u64 headers_end = 0;
    auto parsed = parse_response_headers<A>(bytes, headers_end);
    if(!parsed.ok()) return Err::err(spp::move(parsed.unwrap_err()));

    Response<A> resp = spp::move(parsed.unwrap());
    auto content_length = resp.find_header("Content-Length"_v);
    u64 body_len = 0;
    if(content_length.ok()) {
        auto n = detail::parse_decimal_u64(*content_length);
        if(!n.ok()) return Err::err(spp::move(n.unwrap_err()));
        body_len = n.unwrap();
    }

    if(headers_end + body_len > bytes.length()) return Err::err("http_body_truncated"_v);
    resp.body = Slice<const u8>{bytes.data() + headers_end, body_len};
    return Err::ok(spp::move(resp));
}

// Reads from `stream` until the headers terminator (\r\n\r\n) is seen and
// returns the parsed response headers (body is empty). `headers_end` is set to
// the byte offset immediately following the terminator, i.e. the start of the
// response body. The caller can continue reading from `stream` at that boundary
// for chunked or Content-Length bodies.
template<typename S, Allocator A = Mdefault>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Response<A>, String_View>
read_response_headers(S& stream, Vec<u8, A>& buf, u64& headers_end) noexcept {
    using Err = Result<Response<A>, String_View>;
    headers_end = 0;
    u8 chunk[2048];
    for(;;) {
        auto got = stream.recv_result(Slice<u8>{chunk, sizeof(chunk)});
        if(!got.ok()) return Err::err(spp::move(got.unwrap_err()));
        u64 n = got.unwrap();
        if(n == 0) return Err::err("http_eof_in_headers"_v);
        for(u64 i = 0; i < n; i++) buf.push(chunk[i]);
        // Look for the terminator across the new bytes and the few before them.
        u64 search_from = buf.length() - n > 3 ? buf.length() - n - 3 : 0;
        for(u64 i = search_from; i + 3 < buf.length(); i++) {
            if(buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
               buf[i + 3] == '\n') {
                headers_end = i + 4;
                break;
            }
        }
        if(headers_end > 0) break;
    }
    return parse_response_headers<A>(buf.slice(), headers_end);
}

// Reads from `stream` until the headers terminator (\r\n\r\n) is seen, then
// reads the body using Content-Length. Returns the full byte buffer plus the
// parsed Response (whose String_Views borrow from the buffer). `stream` may be
// any Net::Byte_Stream: plain TCP, a TLS-wrapped Stream, an in-memory fake.
template<Allocator A = Mdefault>
struct Fetched_Response {
    Vec<u8, A> raw;
    Response<A> response;
};

template<typename S, Allocator A = Mdefault>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Fetched_Response<A>, String_View>
fetch(S& stream, const Request<A>& request) noexcept {
    using Err = Result<Fetched_Response<A>, String_View>;
    auto wire = request.template to_bytes<A>();
    auto sent = stream.send_all_result(wire.slice());
    if(!sent.ok()) return Err::err(spp::move(sent.unwrap_err()));

    Vec<u8, A> buf(1024);
    u64 headers_end = 0;
    auto preview = read_response_headers(stream, buf, headers_end);
    if(!preview.ok()) return Err::err(spp::move(preview.unwrap_err()));

    u64 content_length = 0;
    auto cl = preview.unwrap().find_header("Content-Length"_v);
    if(cl.ok()) {
        auto parsed = detail::parse_decimal_u64(*cl);
        if(parsed.ok()) content_length = parsed.unwrap();
    }

    u64 already_have_body = buf.length() - headers_end;
    u8 chunk[2048];
    while(already_have_body < content_length) {
        // Cap each read to (remaining body) so we don't slurp bytes that
        // belong to the next pipelined response on the same connection.
        u64 want = content_length - already_have_body;
        if(want > sizeof(chunk)) want = sizeof(chunk);
        auto got = stream.recv_result(Slice<u8>{chunk, want});
        if(!got.ok()) return Err::err(spp::move(got.unwrap_err()));
        u64 n = got.unwrap();
        if(n == 0) return Err::err("http_eof_in_body"_v);
        for(u64 i = 0; i < n; i++) buf.push(chunk[i]);
        already_have_body += n;
    }

    Fetched_Response<A> out;
    out.raw = spp::move(buf);
    auto resp = parse_response<A>(out.raw.slice());
    if(!resp.ok()) return Err::err(spp::move(resp.unwrap_err()));
    out.response = spp::move(resp.unwrap());
    return Err::ok(spp::move(out));
}

} // namespace spp::Protocol::Http
