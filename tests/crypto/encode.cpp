#include "test.h"

#include <spp/crypto/encode.h>

i32 main() {
    Test test{"empty"_v};

    // Hex encode + decode round trips and a canonical vector.
    {
        u8 data[] = {0x00, 0x01, 0x02, 0x10, 0xab, 0xcd, 0xef, 0xff};
        auto enc = Crypto::hex_encode<Mdefault>(Slice<const u8>{data, 8});
        assert(enc == "00010210abcdefff"_v);
        auto upper = Crypto::hex_encode<Mdefault>(Slice<const u8>{data, 8}, true);
        assert(upper == "00010210ABCDEFFF"_v);

        auto back = Crypto::hex_decode<Mdefault>(enc.view());
        assert(back.ok());
        assert(back.unwrap().length() == 8);
        for(u64 i = 0; i < 8; i++) assert(back.unwrap()[i] == data[i]);

        auto bad_len = Crypto::hex_decode<Mdefault>("0a1"_v);
        assert(!bad_len.ok());
        auto bad_char = Crypto::hex_decode<Mdefault>("0g"_v);
        assert(!bad_char.ok());
    }

    // RFC 4648 §10 base64 test vectors.
    {
        auto e = [](String_View s) {
            return Crypto::base64_encode<Mdefault>(Slice<const u8>{s.data(), s.length()});
        };
        assert(e(""_v) == ""_v);
        assert(e("f"_v) == "Zg=="_v);
        assert(e("fo"_v) == "Zm8="_v);
        assert(e("foo"_v) == "Zm9v"_v);
        assert(e("foob"_v) == "Zm9vYg=="_v);
        assert(e("fooba"_v) == "Zm9vYmE="_v);
        assert(e("foobar"_v) == "Zm9vYmFy"_v);
    }

    // URL-safe base64: no padding, `-_` alphabet. Encode bytes that hit the
    // standard alphabet's `+/` positions to prove alphabet substitution.
    {
        u8 raw[3] = {0xfb, 0xff, 0xbf};
        // Standard alphabet emits `+/+/`-ish bytes; url-safe must avoid them.
        auto std_enc = Crypto::base64_encode<Mdefault>(Slice<const u8>{raw, 3}, false);
        auto url_enc = Crypto::base64_encode<Mdefault>(Slice<const u8>{raw, 3}, true);
        assert(std_enc == "+/+/"_v);
        assert(url_enc == "-_-_"_v);

        u8 odd[1] = {0xff};
        auto url_odd = Crypto::base64_encode<Mdefault>(Slice<const u8>{odd, 1}, true);
        assert(url_odd == "_w"_v); // no padding
    }

    // URL percent-encoding: unreserved characters stay, others get %XX uppercase.
    {
        // Binance-style query parameter content.
        auto e = Crypto::url_encode<Mdefault>("symbol=BTCUSDT&side=BUY price=10000.00"_v);
        assert(e == "symbol%3DBTCUSDT%26side%3DBUY%20price%3D10000.00"_v);

        auto unreserved = Crypto::url_encode<Mdefault>("ABCxyz0-9_.~"_v);
        assert(unreserved == "ABCxyz0-9_.~"_v);
    }
    return 0;
}
