#pragma once

#include <spp/core/base.h>

namespace spp::Crypto {

// Hex encoding (RFC 4648 §8). Lowercase by default.
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> hex_encode(Slice<const u8> in, bool upper = false) noexcept {
    static constexpr const char* lo = "0123456789abcdef";
    static constexpr const char* up = "0123456789ABCDEF";
    const char* tab = upper ? up : lo;

    String<A> out{in.length() * 2};
    out.set_length(in.length() * 2);
    for(u64 i = 0; i < in.length(); i++) {
        out[i * 2 + 0] = static_cast<u8>(tab[(in[i] >> 4) & 0x0F]);
        out[i * 2 + 1] = static_cast<u8>(tab[in[i] & 0x0F]);
    }
    return out;
}

template<Allocator A = Mdefault>
[[nodiscard]] inline Result<Vec<u8, A>, String_View> hex_decode(String_View in) noexcept {
    if(in.length() % 2 != 0) return Result<Vec<u8, A>, String_View>::err("hex_odd_length"_v);
    Vec<u8, A> out(in.length() / 2);
    auto nibble = [](u8 c) -> i32 {
        if(c >= '0' && c <= '9') return c - '0';
        if(c >= 'a' && c <= 'f') return c - 'a' + 10;
        if(c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for(u64 i = 0; i < in.length(); i += 2) {
        i32 hi = nibble(in[i]);
        i32 lo = nibble(in[i + 1]);
        if(hi < 0 || lo < 0) return Result<Vec<u8, A>, String_View>::err("hex_invalid_char"_v);
        out.push(static_cast<u8>((hi << 4) | lo));
    }
    return Result<Vec<u8, A>, String_View>::ok(spp::move(out));
}

// RFC 4648 Base64. `url_safe = true` uses the `-_` alphabet and omits padding.
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> base64_encode(Slice<const u8> in, bool url_safe = false) noexcept {
    static constexpr const char* std_alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static constexpr const char* url_alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* alpha = url_safe ? url_alpha : std_alpha;

    u64 full_triples = in.length() / 3;
    u64 rem = in.length() - full_triples * 3;
    u64 out_len = full_triples * 4 + (rem ? (url_safe ? (rem + 1) : 4) : 0);

    String<A> out{out_len};
    out.set_length(out_len);

    u64 o = 0;
    for(u64 i = 0; i < full_triples; i++) {
        u32 v = (static_cast<u32>(in[i * 3 + 0]) << 16) |
                (static_cast<u32>(in[i * 3 + 1]) << 8) | static_cast<u32>(in[i * 3 + 2]);
        out[o++] = static_cast<u8>(alpha[(v >> 18) & 0x3F]);
        out[o++] = static_cast<u8>(alpha[(v >> 12) & 0x3F]);
        out[o++] = static_cast<u8>(alpha[(v >> 6) & 0x3F]);
        out[o++] = static_cast<u8>(alpha[v & 0x3F]);
    }
    if(rem == 1) {
        u32 v = static_cast<u32>(in[full_triples * 3]) << 16;
        out[o++] = static_cast<u8>(alpha[(v >> 18) & 0x3F]);
        out[o++] = static_cast<u8>(alpha[(v >> 12) & 0x3F]);
        if(!url_safe) {
            out[o++] = '=';
            out[o++] = '=';
        }
    } else if(rem == 2) {
        u32 v = (static_cast<u32>(in[full_triples * 3 + 0]) << 16) |
                (static_cast<u32>(in[full_triples * 3 + 1]) << 8);
        out[o++] = static_cast<u8>(alpha[(v >> 18) & 0x3F]);
        out[o++] = static_cast<u8>(alpha[(v >> 12) & 0x3F]);
        out[o++] = static_cast<u8>(alpha[(v >> 6) & 0x3F]);
        if(!url_safe) out[o++] = '=';
    }
    return out;
}

// RFC 3986 §2.3 percent-encoding. Only unreserved characters are passed through;
// everything else becomes %XX (uppercase hex). Exchange APIs uniformly expect
// this form for query-string components.
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> url_encode(String_View in) noexcept {
    auto unreserved = [](u8 c) -> bool {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '.' || c == '_' || c == '~';
    };

    u64 out_len = 0;
    for(u8 c : in) out_len += unreserved(c) ? 1 : 3;

    String<A> out{out_len};
    out.set_length(out_len);
    static constexpr const char* hex = "0123456789ABCDEF";
    u64 o = 0;
    for(u8 c : in) {
        if(unreserved(c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = static_cast<u8>(hex[(c >> 4) & 0x0F]);
            out[o++] = static_cast<u8>(hex[c & 0x0F]);
        }
    }
    return out;
}

} // namespace spp::Crypto
