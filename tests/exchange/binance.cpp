#include "test.h"

#include <spp/exchange/binance.h>

namespace Bnc = Exchange::Binance;

i32 main() {
    Test test{"empty"_v};

    Trace("Binance SIGNED request canonical example") {
        // Vector reproduced from Binance Spot REST docs § "SIGNED Endpoint Examples".
        //   secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"
        //   query  = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559"
        //   HMAC_SHA256(query, secret) =
        //     c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71
        String_View secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"_v;
        String_View query =
            "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
            "&recvWindow=5000&timestamp=1499827319559"_v;
        auto sig = Bnc::sign<Mdefault>(secret, query);
        assert(sig == "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71"_v);
    }

    Trace("Binance signed_request_full appends signature and X-MBX-APIKEY") {
        String_View secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"_v;
        String_View key = "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A"_v;
        String_View query =
            "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
            "&recvWindow=5000&timestamp=1499827319559"_v;

        auto signed_req = Bnc::signed_request_full<Mdefault>(
            "POST"_v, "/api/v3/order"_v, query, Bnc::k_spot_host, key, secret);

        assert(signed_req.request.method == "POST"_v);
        assert(signed_req.request.host == "api.binance.com"_v);

        // Path must end with &signature=<64 hex>. Tail length = 11 + 64 = 75.
        String_View p = signed_req.request.path;
        assert(p.length() > 75);
        assert(p.sub(p.length() - 75, p.length()) ==
               "&signature=c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71"_v);

        // Headers must include X-MBX-APIKEY: <key>.
        bool found = false;
        for(const auto& h : signed_req.request.headers) {
            if(h.name == "X-MBX-APIKEY"_v && h.value == key) found = true;
        }
        assert(found);

        // Wire serialization references must remain valid via owned_path.
        auto wire = signed_req.request.to_bytes<Mdefault>();
        assert(wire.length() > 0);
    }

    Trace("Binance Query_Builder url-encodes values") {
        Bnc::Query_Builder<Mdefault> q;
        q.add("symbol"_v, "BTCUSDT"_v);
        q.add("note"_v, "hello world"_v);
        // 'hello world' becomes 'hello%20world'; '&' separator between params.
        assert(q.view() == "symbol=BTCUSDT&note=hello%20world"_v);
    }

    return 0;
}
