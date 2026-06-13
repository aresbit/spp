#pragma once

#include <spp/core/base.h>

namespace spp::Crypto {

// FIPS 180-4 SHA-256. Streaming Init/Update/Final and one-shot helpers.
// No external dependencies; portable byte-oriented implementation.
struct SHA256 {
    static constexpr u64 block_size = 64;
    static constexpr u64 digest_size = 32;

    struct Digest {
        u8 bytes[digest_size]{};
    };

    SHA256() noexcept {
        reset();
    }

    void reset() noexcept {
        h_[0] = 0x6a09e667u;
        h_[1] = 0xbb67ae85u;
        h_[2] = 0x3c6ef372u;
        h_[3] = 0xa54ff53au;
        h_[4] = 0x510e527fu;
        h_[5] = 0x9b05688cu;
        h_[6] = 0x1f83d9abu;
        h_[7] = 0x5be0cd19u;
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
        // Pad: append 0x80, then zeros, then 64-bit big-endian total bit length.
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
        for(u64 i = 0; i < 8; i++) {
            out.bytes[i * 4 + 0] = static_cast<u8>(h_[i] >> 24);
            out.bytes[i * 4 + 1] = static_cast<u8>(h_[i] >> 16);
            out.bytes[i * 4 + 2] = static_cast<u8>(h_[i] >> 8);
            out.bytes[i * 4 + 3] = static_cast<u8>(h_[i]);
        }
        reset();
        return out;
    }

    [[nodiscard]] static Digest hash(Slice<const u8> in) noexcept {
        SHA256 s;
        s.update(in);
        return s.finalize();
    }

    [[nodiscard]] static Digest hash(String_View in) noexcept {
        return hash(Slice<const u8>{in.data(), in.length()});
    }

private:
    static constexpr u32 k_[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    [[nodiscard]] static constexpr u32 ror_(u32 x, u32 n) noexcept {
        return (x >> n) | (x << (32 - n));
    }

    void transform_(const u8* block) noexcept {
        u32 w[64];
        for(u64 i = 0; i < 16; i++) {
            w[i] = (static_cast<u32>(block[i * 4 + 0]) << 24) |
                   (static_cast<u32>(block[i * 4 + 1]) << 16) |
                   (static_cast<u32>(block[i * 4 + 2]) << 8) |
                   (static_cast<u32>(block[i * 4 + 3]));
        }
        for(u64 i = 16; i < 64; i++) {
            u32 s0 = ror_(w[i - 15], 7) ^ ror_(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = ror_(w[i - 2], 17) ^ ror_(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        u32 e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

        for(u64 i = 0; i < 64; i++) {
            u32 S1 = ror_(e, 6) ^ ror_(e, 11) ^ ror_(e, 25);
            u32 ch = (e & f) ^ (~e & g);
            u32 t1 = hh + S1 + ch + k_[i] + w[i];
            u32 S0 = ror_(a, 2) ^ ror_(a, 13) ^ ror_(a, 22);
            u32 mj = (a & b) ^ (a & c) ^ (b & c);
            u32 t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += hh;
    }

    u32 h_[8]{};
    u8 buf_[block_size]{};
    u64 buf_len_ = 0;
    u64 total_bytes_ = 0;
};

} // namespace spp::Crypto
