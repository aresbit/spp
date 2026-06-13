#include "test.h"

#include <spp/crypto/hmac.h>
#include <spp/crypto/sha256.h>

static bool digest_equals(const Crypto::SHA256::Digest& d, String_View hex) {
    auto to_hex = [](u8 b) -> char {
        return static_cast<char>(b < 10 ? '0' + b : 'a' + (b - 10));
    };
    if(hex.length() != 64) return false;
    for(u64 i = 0; i < 32; i++) {
        if(hex[i * 2 + 0] != static_cast<u8>(to_hex((d.bytes[i] >> 4) & 0x0F))) return false;
        if(hex[i * 2 + 1] != static_cast<u8>(to_hex(d.bytes[i] & 0x0F))) return false;
    }
    return true;
}

i32 main() {
    Test test{"empty"_v};

    // FIPS 180-4 published vectors.
    {
        auto d = Crypto::SHA256::hash(""_v);
        assert(digest_equals(
            d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"_v));
    }
    {
        auto d = Crypto::SHA256::hash("abc"_v);
        assert(digest_equals(
            d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"_v));
    }
    {
        auto d = Crypto::SHA256::hash(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"_v);
        assert(digest_equals(
            d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"_v));
    }
    // Streaming consistency: split a payload across update() boundaries and
    // verify the digest matches the one-shot path. Catches buffer-flush bugs.
    {
        String_View msg = "The quick brown fox jumps over the lazy dog"_v;
        Crypto::SHA256 s;
        s.update(msg.sub(0, 7));
        s.update(msg.sub(7, msg.length()));
        auto a = s.finalize();
        auto b = Crypto::SHA256::hash(msg);
        for(u64 i = 0; i < 32; i++) assert(a.bytes[i] == b.bytes[i]);
        assert(digest_equals(
            a, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"_v));
    }

    // RFC 4231 HMAC-SHA256 test vector #1.
    {
        u8 key[20]{};
        for(u64 i = 0; i < 20; i++) key[i] = 0x0b;
        String_View msg = "Hi There"_v;
        auto d = Crypto::HMAC_SHA256::sign(Slice<const u8>{key, 20},
                                           Slice<const u8>{msg.data(), msg.length()});
        assert(digest_equals(
            d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"_v));
    }
    // RFC 4231 test vector #2 (key shorter than block).
    {
        String_View key = "Jefe"_v;
        String_View msg = "what do ya want for nothing?"_v;
        auto d = Crypto::HMAC_SHA256::sign(key, msg);
        assert(digest_equals(
            d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"_v));
    }
    // RFC 4231 test vector #4 (25-byte key, 50-byte 0xcd payload).
    {
        u8 key[25];
        for(u64 i = 0; i < 25; i++) key[i] = static_cast<u8>(i + 1);
        u8 msg[50];
        for(u64 i = 0; i < 50; i++) msg[i] = 0xcd;
        auto d = Crypto::HMAC_SHA256::sign(Slice<const u8>{key, 25},
                                           Slice<const u8>{msg, 50});
        assert(digest_equals(
            d, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"_v));
    }
    // Long-key reduction path: key > block_size triggers key = SHA256(key).
    // RFC 4231 test vector #6 (131-byte key, short ASCII payload).
    {
        u8 long_key[131];
        for(u64 i = 0; i < 131; i++) long_key[i] = 0xaa;
        String_View msg = "Test Using Larger Than Block-Size Key - Hash Key First"_v;
        auto d = Crypto::HMAC_SHA256::sign(Slice<const u8>{long_key, 131},
                                           Slice<const u8>{msg.data(), msg.length()});
        assert(digest_equals(
            d, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"_v));
    }
    return 0;
}
