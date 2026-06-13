#pragma once

#include <spp/core/base.h>

namespace spp::Crypto {

// FIPS 180-4 SHA-1. Used here only for the WebSocket handshake accept value
// (RFC 6455 §4.2.2) — SHA-1 is cryptographically broken for collision
// resistance and must not be used for new security designs.
struct SHA1 {
    static constexpr u64 block_size = 64;
    static constexpr u64 digest_size = 20;

    struct Digest {
        u8 bytes[digest_size]{};
    };

    SHA1() noexcept {
        reset();
    }

    void reset() noexcept {
        h_[0] = 0x67452301u;
        h_[1] = 0xefcdab89u;
        h_[2] = 0x98badcfeu;
        h_[3] = 0x10325476u;
        h_[4] = 0xc3d2e1f0u;
        buf_len_ = 0;
        total_bytes_ = 0;
    }

    void update(Slice<const u8> in) noexcept {
        total_bytes_ += in.length();
        u64 i = 0;
        if(buf_len_ > 0) {
            u64 take = block_size - buf_len_;
            if(take > in.length()) take = in.length();
            for(u64 k = 0; k < take; k++) buf_[buf_len_ + k] = in[k];
            buf_len_ += take;
            i += take;
            if(buf_len_ == block_size) {
                transform_(buf_);
                buf_len_ = 0;
            }
        }
        while(i + block_size <= in.length()) {
            transform_(in.data() + i);
            i += block_size;
        }
        while(i < in.length()) {
            buf_[buf_len_++] = in[i++];
        }
    }

    void update(String_View in) noexcept {
        update(Slice<const u8>{in.data(), in.length()});
    }

    [[nodiscard]] Digest finalize() noexcept {
        u64 bit_len = total_bytes_ * 8;
        buf_[buf_len_++] = 0x80;
        if(buf_len_ > block_size - 8) {
            while(buf_len_ < block_size) buf_[buf_len_++] = 0;
            transform_(buf_);
            buf_len_ = 0;
        }
        while(buf_len_ < block_size - 8) buf_[buf_len_++] = 0;
        for(u64 i = 0; i < 8; i++) {
            buf_[block_size - 1 - i] = static_cast<u8>(bit_len >> (8 * i));
        }
        transform_(buf_);

        Digest out;
        for(u64 i = 0; i < 5; i++) {
            out.bytes[i * 4 + 0] = static_cast<u8>(h_[i] >> 24);
            out.bytes[i * 4 + 1] = static_cast<u8>(h_[i] >> 16);
            out.bytes[i * 4 + 2] = static_cast<u8>(h_[i] >> 8);
            out.bytes[i * 4 + 3] = static_cast<u8>(h_[i]);
        }
        reset();
        return out;
    }

    [[nodiscard]] static Digest hash(Slice<const u8> in) noexcept {
        SHA1 s;
        s.update(in);
        return s.finalize();
    }
    [[nodiscard]] static Digest hash(String_View in) noexcept {
        return hash(Slice<const u8>{in.data(), in.length()});
    }

private:
    [[nodiscard]] static constexpr u32 rol_(u32 x, u32 n) noexcept {
        return (x << n) | (x >> (32 - n));
    }

    void transform_(const u8* block) noexcept {
        u32 w[80];
        for(u64 i = 0; i < 16; i++) {
            w[i] = (static_cast<u32>(block[i * 4 + 0]) << 24) |
                   (static_cast<u32>(block[i * 4 + 1]) << 16) |
                   (static_cast<u32>(block[i * 4 + 2]) << 8) |
                   (static_cast<u32>(block[i * 4 + 3]));
        }
        for(u64 i = 16; i < 80; i++) {
            w[i] = rol_(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

        for(u64 i = 0; i < 80; i++) {
            u32 f = 0, k = 0;
            if(i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999u;
            } else if(i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if(i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            u32 t = rol_(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol_(b, 30);
            b = a;
            a = t;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    u32 h_[5]{};
    u8 buf_[block_size]{};
    u64 buf_len_ = 0;
    u64 total_bytes_ = 0;
};

} // namespace spp::Crypto
