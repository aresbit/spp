#pragma once

#include <spp/core/base.h>
#include <spp/io/stream.h>
#include <spp/protocol/http.h>

namespace spp::Protocol::Sse {

// Minimal Server-Sent Events (SSE) reader. Sits on top of any Net::Byte_Stream
// that is positioned at the start of an HTTP response body. The response is
// expected to use chunked transfer encoding (the common case for SSE APIs such
// as the Anthropic Messages API). The reader decodes chunks incrementally and
// yields one Message per SSE event.
//
// String_Views inside Message borrow from the reader's internal buffers; they
// are valid only until the next call to next().

struct Message {
    String_View event = "message"_v;
    String_View data;
    String_View id;
    Opt<u64> retry;
    bool done = false;
};

namespace detail {

[[nodiscard]] inline u64 find_lf(Slice<const u8> s, u64 from) noexcept {
    for(u64 i = from; i < s.length(); i++) {
        if(s[i] == '\n') return i;
    }
    return s.length();
}

[[nodiscard]] inline String_View strip_leading_space(String_View s) noexcept {
    u64 i = 0;
    while(i < s.length() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.sub(i, s.length());
}

[[nodiscard]] inline Result<u64, String_View> parse_decimal_u64(String_View s) noexcept {
    if(s.empty()) return Result<u64, String_View>::err("sse_bad_number"_v);
    u64 v = 0;
    for(u8 c : s) {
        if(c < '0' || c > '9') return Result<u64, String_View>::err("sse_bad_number"_v);
        v = v * 10 + (c - '0');
    }
    return Result<u64, String_View>::ok(spp::move(v));
}

[[nodiscard]] inline Result<u64, String_View> parse_hex_u64(String_View s) noexcept {
    if(s.empty()) return Result<u64, String_View>::err("sse_bad_hex"_v);
    u64 v = 0;
    for(u8 c : s) {
        u64 digit;
        if(c >= '0' && c <= '9') {
            digit = c - '0';
        } else if(c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if(c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return Result<u64, String_View>::err("sse_bad_hex"_v);
        }
        v = (v << 4) | digit;
    }
    return Result<u64, String_View>::ok(spp::move(v));
}

[[nodiscard]] inline bool contains_ci(String_View haystack, String_View needle) noexcept {
    if(needle.length() > haystack.length()) return false;
    for(u64 i = 0; i + needle.length() <= haystack.length(); i++) {
        bool match = true;
        for(u64 j = 0; j < needle.length(); j++) {
            u8 a = haystack[i + j];
            u8 b = needle[j];
            if(a >= 'A' && a <= 'Z') a += 32;
            if(b >= 'A' && b <= 'Z') b += 32;
            if(a != b) {
                match = false;
                break;
            }
        }
        if(match) return true;
    }
    return false;
}

} // namespace detail

template<typename S, Allocator A = Mdefault>
    requires Net::Byte_Stream<S>
struct Reader {
    explicit Reader(S& stream) noexcept : stream_(&stream) {
    }

    Reader(S& stream, Slice<const u8> initial) noexcept : stream_(&stream) {
        for(u8 c : initial) buf_.push(c);
    }

    [[nodiscard]] Result<Message, String_View> next() noexcept {
        using R = Result<Message, String_View>;
        data_buf_.clear();
        id_buf_.clear();
        retry_ = Opt<u64>{}; // ponytail: `= {}` would pick Opt's variadic ctor and set ok_=true

        for(;;) {
            compact();
            // Parse complete SSE lines from the buffered chunk stream.
            u64 line_start = buf_cursor_;
            while(line_start < buf_.length()) {
                u64 line_end = detail::find_lf(buf_.slice(), line_start);
                if(line_end >= buf_.length()) break; // incomplete line

                u64 end = line_end;
                if(end > line_start && buf_[end - 1] == '\r') end--;

                if(end == line_start) {
                    // Empty line terminates an event.
                    buf_cursor_ = line_end + 1;
                    if(data_buf_.length() > 0 || event_buf_.length() > 0 ||
                       id_buf_.length() > 0 || retry_.ok()) {
                        return R::ok(build_message());
                    }
                    line_start = buf_cursor_;
                    continue;
                }

                // Comments start with ':' and are ignored.
                if(buf_[line_start] != ':') {
                    u64 colon = line_start;
                    while(colon < end && buf_[colon] != ':') colon++;
                    String_View name{buf_.data() + line_start, colon - line_start};
                    String_View value;
                    if(colon < end) {
                        value = String_View{buf_.data() + colon + 1, end - colon - 1};
                        value = detail::strip_leading_space(value);
                    }

                    if(name == "event"_v) {
                        event_buf_.clear();
                        for(u8 c : value) event_buf_.push(c);
                        event_ = String_View{event_buf_.data(), event_buf_.length()};
                    } else if(name == "data"_v) {
                        if(data_buf_.length() > 0) data_buf_.push('\n');
                        for(u8 c : value) data_buf_.push(c);
                    } else if(name == "id"_v) {
                        id_buf_.clear();
                        for(u8 c : value) id_buf_.push(c);
                    } else if(name == "retry"_v) {
                        auto n = detail::parse_decimal_u64(value);
                        if(n.ok()) retry_ = Opt<u64>{n.unwrap()};
                    }
                }

                buf_cursor_ = line_end + 1;
                line_start = buf_cursor_;
            }

            // Need more bytes: decode the next HTTP chunk.
            auto fed = feed_chunk();
            if(!fed.ok()) return R::err(spp::move(fed.unwrap_err()));
            if(fed.unwrap() == 0) {
                // Chunk terminator. Dispatch any pending event, then EOF.
                if(data_buf_.length() > 0 || event_buf_.length() > 0 ||
                   id_buf_.length() > 0 || retry_.ok()) {
                    return R::ok(build_message());
                }
                return R::err("sse_eof"_v);
            }
        }
    }

private:
    S* stream_;
    Vec<u8, A> buf_;
    u64 buf_cursor_ = 0;
    Vec<u8, A> data_buf_;
    Vec<u8, A> event_buf_;
    Vec<u8, A> id_buf_;
    String_View event_ = "message"_v;
    Opt<u64> retry_;

    [[nodiscard]] Message build_message() noexcept {
        Message msg;
        msg.event = event_.length() > 0 ? event_ : "message"_v;
        msg.data = String_View{data_buf_.data(), data_buf_.length()};
        msg.id = String_View{id_buf_.data(), id_buf_.length()};
        msg.retry = retry_;
        msg.done = (msg.data == "[DONE]"_v);
        event_buf_.clear();
        event_ = "message"_v;
        return msg;
    }

    [[nodiscard]] Result<u8, String_View> read_byte() noexcept {
        u8 b;
        auto r = stream_->recv_result(Slice<u8>{&b, 1});
        if(!r.ok()) return Result<u8, String_View>::err(spp::move(r.unwrap_err()));
        if(r.unwrap() == 0) return Result<u8, String_View>::err("sse_eof"_v);
        return Result<u8, String_View>::ok(spp::move(b));
    }

    [[nodiscard]] Result<u64, String_View> feed_chunk() noexcept {
        using R = Result<u64, String_View>;

        Vec<u8, A> line(16);
        for(;;) {
            auto b = read_byte();
            if(!b.ok()) return R::err(spp::move(b.unwrap_err()));
            u8 c = b.unwrap();
            if(c == '\r') {
                auto nl = read_byte();
                if(!nl.ok()) return R::err(spp::move(nl.unwrap_err()));
                if(nl.unwrap() != '\n') return R::err("http_chunk_bad_term"_v);
                break;
            }
            if(c == '\n') return R::err("http_chunk_bad_term"_v);
            line.push(c);
        }

        u64 hex_len = line.length();
        for(u64 i = 0; i < line.length(); i++) {
            if(line[i] == ';') {
                hex_len = i;
                break;
            }
        }
        if(hex_len == 0) {
            // Terminator chunk. Consume the final CRLF (no trailers expected).
            auto cr = read_byte();
            if(!cr.ok()) return R::err(spp::move(cr.unwrap_err()));
            if(cr.unwrap() != '\r') return R::err("http_chunk_bad_term"_v);
            auto lf = read_byte();
            if(!lf.ok()) return R::err(spp::move(lf.unwrap_err()));
            if(lf.unwrap() != '\n') return R::err("http_chunk_bad_term"_v);
            return R::ok(0);
        }

        auto size = detail::parse_hex_u64(String_View{line.data(), hex_len});
        if(!size.ok()) return R::err(spp::move(size.unwrap_err()));
        u64 n = size.unwrap();

        u64 want = n;
        while(want > 0) {
            u8 tmp[1024];
            u64 take = want < sizeof(tmp) ? want : sizeof(tmp);
            auto got = stream_->recv_result(Slice<u8>{tmp, take});
            if(!got.ok()) return R::err(spp::move(got.unwrap_err()));
            u64 got_n = got.unwrap();
            if(got_n == 0) return R::err("sse_eof"_v);
            for(u64 i = 0; i < got_n; i++) buf_.push(tmp[i]);
            want -= got_n;
        }

        auto cr = read_byte();
        if(!cr.ok()) return R::err(spp::move(cr.unwrap_err()));
        if(cr.unwrap() != '\r') return R::err("http_chunk_bad_term"_v);
        auto lf = read_byte();
        if(!lf.ok()) return R::err(spp::move(lf.unwrap_err()));
        if(lf.unwrap() != '\n') return R::err("http_chunk_bad_term"_v);

        return R::ok(spp::move(n));
    }

    void compact() noexcept {
        u64 remaining = buf_.length() - buf_cursor_;
        if(buf_cursor_ > 4096 && remaining < buf_cursor_) {
            for(u64 i = 0; i < remaining; i++) buf_[i] = buf_[buf_cursor_ + i];
            buf_.resize(remaining);
            buf_cursor_ = 0;
        }
    }
};

// Sends `request` on `stream`, reads the response headers, validates that the
// response is an SSE stream, and returns a Reader positioned at the start of
// the chunked response body. Any body bytes already read while consuming the
// headers are passed to the reader so they are not lost.
template<typename S, Allocator A = Mdefault>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Reader<S, A>, String_View>
open(S& stream, const Http::Request<A>& request) noexcept {
    using Err = Result<Reader<S, A>, String_View>;

    auto wire = request.template to_bytes<A>();
    auto sent = stream.send_all_result(wire.slice());
    if(!sent.ok()) return Err::err(spp::move(sent.unwrap_err()));

    Vec<u8, A> buf(1024);
    u64 headers_end = 0;
    auto headers = Http::read_response_headers(stream, buf, headers_end);
    if(!headers.ok()) return Err::err(spp::move(headers.unwrap_err()));

    auto& resp = headers.unwrap();
    if(resp.status_code != 200) return Err::err("sse_bad_status"_v);

    auto ct = resp.find_header("Content-Type"_v);
    if(!ct.ok() || !detail::contains_ci(*ct, "text/event-stream"_v)) {
        return Err::err("sse_not_event_stream"_v);
    }

    Slice<const u8> body_start{buf.data() + headers_end, buf.length() - headers_end};
    return Err::ok(Reader<S, A>{stream, body_start});
}

} // namespace spp::Protocol::Sse
