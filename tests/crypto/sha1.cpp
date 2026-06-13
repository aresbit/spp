#include "test.h"

#include <spp/crypto/sha1.h>

static bool digest_equals(const Crypto::SHA1::Digest& d, String_View hex) {
    auto to_hex = [](u8 b) -> char {
        return static_cast<char>(b < 10 ? '0' + b : 'a' + (b - 10));
    };
    if(hex.length() != 40) return false;
    for(u64 i = 0; i < 20; i++) {
        if(hex[i * 2 + 0] != static_cast<u8>(to_hex((d.bytes[i] >> 4) & 0x0F))) return false;
        if(hex[i * 2 + 1] != static_cast<u8>(to_hex(d.bytes[i] & 0x0F))) return false;
    }
    return true;
}

i32 main() {
    Test test{"empty"_v};

    // FIPS 180-4 vectors.
    {
        auto d = Crypto::SHA1::hash(""_v);
        assert(digest_equals(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"_v));
    }
    {
        auto d = Crypto::SHA1::hash("abc"_v);
        assert(digest_equals(d, "a9993e364706816aba3e25717850c26c9cd0d89d"_v));
    }
    {
        auto d = Crypto::SHA1::hash(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"_v);
        assert(digest_equals(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"_v));
    }
    // Streaming consistency across an update boundary.
    {
        String_View msg = "The quick brown fox jumps over the lazy dog"_v;
        Crypto::SHA1 s;
        s.update(msg.sub(0, 10));
        s.update(msg.sub(10, msg.length()));
        auto a = s.finalize();
        auto b = Crypto::SHA1::hash(msg);
        for(u64 i = 0; i < 20; i++) assert(a.bytes[i] == b.bytes[i]);
        assert(digest_equals(a, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"_v));
    }
    return 0;
}
