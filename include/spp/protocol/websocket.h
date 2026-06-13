#pragma once

#include <spp/core/base.h>
#include <spp/crypto/encode.h>
#include <spp/crypto/sha1.h>
#include <spp/io/stream.h>

namespace spp::Protocol::Websocket {

// RFC 6455 frame codec. Pure encode/decode logic — the calling code is
// responsible for the actual TCP/TLS I/O. This split lets the codec be
// re-used with any byte transport (raw TCP, mbedtls, a sidecar's domain
// socket, an in-process loopback test).

enum struct Opcode : u8 {
    cont = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xa,
};

struct Frame {
    bool fin = true;
    Opcode op = Opcode::binary;
    Slice<const u8> payload;
};

namespace detail {

[[nodiscard]] constexpr u8 opcode_byte(bool fin, Opcode op) noexcept {
    return static_cast<u8>((fin ? 0x80u : 0x00u) | (static_cast<u8>(op) & 0x0fu));
}

inline void apply_mask(u8* data, u64 length, const u8 mask[4]) noexcept {
    for(u64 i = 0; i < length; i++) data[i] ^= mask[i & 3];
}

} // namespace detail

// Encodes a frame into `out` (appended). When `masked` is true, the caller
// must supply a per-frame 32-bit mask; client-to-server traffic per RFC 6455
// must be masked.
template<Allocator A>
inline void encode(Vec<u8, A>& out, const Frame& frame, bool masked = false,
                   const u8 mask[4] = nullptr) noexcept {
    out.push(detail::opcode_byte(frame.fin, frame.op));

    u64 len = frame.payload.length();
    u8 mask_bit = masked ? 0x80u : 0x00u;
    if(len < 126) {
        out.push(static_cast<u8>(mask_bit | static_cast<u8>(len)));
    } else if(len <= 0xFFFFu) {
        out.push(static_cast<u8>(mask_bit | 126));
        out.push(static_cast<u8>((len >> 8) & 0xff));
        out.push(static_cast<u8>(len & 0xff));
    } else {
        out.push(static_cast<u8>(mask_bit | 127));
        for(i32 i = 7; i >= 0; i--) {
            out.push(static_cast<u8>((len >> (8 * i)) & 0xff));
        }
    }

    if(masked) {
        for(u64 i = 0; i < 4; i++) out.push(mask[i]);
        u64 start = out.length();
        for(u64 i = 0; i < len; i++) out.push(frame.payload[i]);
        detail::apply_mask(out.data() + start, len, mask);
    } else {
        for(u64 i = 0; i < len; i++) out.push(frame.payload[i]);
    }
}

struct Decoded {
    Frame frame;
    u64 consumed = 0; // total wire bytes the frame spanned
};

// Tries to decode one frame from `in`. Returns "need_more" if input is short.
// If the frame was masked, the payload bytes inside `in` are mutated in place
// to be unmasked, and `Decoded::frame.payload` points into `in`. Caller must
// keep `in` alive for the lifetime of the returned Slice.
[[nodiscard]] inline Result<Decoded, String_View> decode(Slice<u8> in) noexcept {
    if(in.length() < 2) return Result<Decoded, String_View>::err("need_more"_v);
    u8 b0 = in[0];
    u8 b1 = in[1];
    bool fin = (b0 & 0x80) != 0;
    Opcode op = static_cast<Opcode>(b0 & 0x0f);
    bool masked = (b1 & 0x80) != 0;
    u64 len = b1 & 0x7f;
    u64 cursor = 2;
    if(len == 126) {
        if(in.length() < cursor + 2) return Result<Decoded, String_View>::err("need_more"_v);
        len = (static_cast<u64>(in[cursor]) << 8) | in[cursor + 1];
        cursor += 2;
    } else if(len == 127) {
        if(in.length() < cursor + 8) return Result<Decoded, String_View>::err("need_more"_v);
        len = 0;
        for(u64 i = 0; i < 8; i++) {
            len = (len << 8) | in[cursor + i];
        }
        cursor += 8;
    }

    u8 mask[4] = {0, 0, 0, 0};
    if(masked) {
        if(in.length() < cursor + 4) return Result<Decoded, String_View>::err("need_more"_v);
        for(u64 i = 0; i < 4; i++) mask[i] = in[cursor + i];
        cursor += 4;
    }

    if(in.length() < cursor + len) return Result<Decoded, String_View>::err("need_more"_v);

    if(masked) {
        detail::apply_mask(in.data() + cursor, len, mask);
    }

    Decoded out;
    out.frame.fin = fin;
    out.frame.op = op;
    out.frame.payload = Slice<const u8>{in.data() + cursor, len};
    out.consumed = cursor + len;
    return Result<Decoded, String_View>::ok(spp::move(out));
}

// Encodes one frame and pushes it through `stream`. Returns the number of
// wire bytes sent on success. `mask` is mandatory for client-to-server
// traffic per RFC 6455 §5.3; pass a per-frame 4-byte random mask.
template<typename S, Allocator A = Mdefault>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<u64, String_View>
send_frame(S& stream, const Frame& frame, bool masked = false,
           const u8 mask[4] = nullptr) noexcept {
    Vec<u8, A> buf;
    encode(buf, frame, masked, mask);
    auto sent = stream.send_all_result(buf.slice());
    if(!sent.ok()) return Result<u64, String_View>::err(spp::move(sent.unwrap_err()));
    return Result<u64, String_View>::ok(static_cast<u64>(buf.length()));
}

// Pulls bytes from `stream` into `buffer` until one full frame is decodable,
// then returns it. `buffer` accumulates across calls so partial frames carry
// over (callers should clear it once they're done with the returned Frame's
// payload, which borrows from `buffer`). consumed_out reports how many bytes
// of `buffer` the returned frame occupies; the caller can compact by erasing
// the prefix or by clearing the buffer entirely between frames.
template<typename S, Allocator A = Mdefault>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Frame, String_View>
recv_frame_into(S& stream, Vec<u8, A>& buffer, u64& consumed_out) noexcept {
    u8 chunk[2048];
    for(;;) {
        auto dec = decode(buffer.slice());
        if(dec.ok()) {
            consumed_out = dec.unwrap().consumed;
            return Result<Frame, String_View>::ok(spp::move(dec.unwrap().frame));
        }
        if(dec.unwrap_err() != "need_more"_v) {
            return Result<Frame, String_View>::err(spp::move(dec.unwrap_err()));
        }
        auto got = stream.recv_result(Slice<u8>{chunk, sizeof(chunk)});
        if(!got.ok()) {
            return Result<Frame, String_View>::err(spp::move(got.unwrap_err()));
        }
        u64 n = got.unwrap();
        if(n == 0) return Result<Frame, String_View>::err("ws_eof"_v);
        for(u64 i = 0; i < n; i++) buffer.push(chunk[i]);
    }
}

// Computes the Sec-WebSocket-Accept value the server is required to send back
// during a handshake (RFC 6455 §4.2.2): base64(SHA-1(client_key + GUID)).
// Client code uses this to verify the server's Sec-WebSocket-Accept header.
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> derive_accept(String_View client_key) noexcept {
    static const String_View kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"_v;
    Crypto::SHA1 sha;
    sha.update(client_key);
    sha.update(kGuid);
    auto digest = sha.finalize();
    return Crypto::base64_encode<A>(
        Slice<const u8>{digest.bytes, Crypto::SHA1::digest_size});
}

} // namespace spp::Protocol::Websocket
