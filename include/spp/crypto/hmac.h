#pragma once

#include <spp/crypto/sha256.h>

namespace spp::Crypto {

// RFC 2104 HMAC, instantiated over a hash that exposes the SHA256 streaming
// API (reset/update/finalize plus block_size/digest_size/Digest). Today only
// HMAC-SHA256 is wired up; the template parameter is left in place so other
// digests can drop in without touching call sites.
template<typename Hash = SHA256>
struct HMAC {
    using Digest = typename Hash::Digest;
    static constexpr u64 block_size = Hash::block_size;
    static constexpr u64 digest_size = Hash::digest_size;

    HMAC() noexcept = default;

    explicit HMAC(Slice<const u8> key) noexcept {
        init(key);
    }
    explicit HMAC(String_View key) noexcept {
        init(Slice<const u8>{key.data(), key.length()});
    }

    void init(Slice<const u8> key) noexcept {
        u8 k_block[block_size]{};
        if(key.length() > block_size) {
            auto digest = Hash::hash(key);
            for(u64 i = 0; i < digest_size; i++) k_block[i] = digest.bytes[i];
        } else {
            for(u64 i = 0; i < key.length(); i++) k_block[i] = key[i];
        }

        u8 ipad[block_size];
        for(u64 i = 0; i < block_size; i++) {
            ipad[i] = k_block[i] ^ 0x36;
            opad_[i] = k_block[i] ^ 0x5c;
        }
        inner_.reset();
        inner_.update(Slice<const u8>{ipad, block_size});
    }

    void update(Slice<const u8> in) noexcept {
        inner_.update(in);
    }
    void update(String_View in) noexcept {
        inner_.update(in);
    }

    [[nodiscard]] Digest finalize() noexcept {
        auto inner_digest = inner_.finalize();
        Hash outer;
        outer.update(Slice<const u8>{opad_, block_size});
        outer.update(Slice<const u8>{inner_digest.bytes, digest_size});
        return outer.finalize();
    }

    [[nodiscard]] static Digest sign(Slice<const u8> key, Slice<const u8> msg) noexcept {
        HMAC h{key};
        h.update(msg);
        return h.finalize();
    }
    [[nodiscard]] static Digest sign(String_View key, String_View msg) noexcept {
        return sign(Slice<const u8>{key.data(), key.length()},
                    Slice<const u8>{msg.data(), msg.length()});
    }

private:
    Hash inner_;
    u8 opad_[block_size]{};
};

using HMAC_SHA256 = HMAC<SHA256>;

} // namespace spp::Crypto
