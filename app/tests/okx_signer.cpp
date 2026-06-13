#include "test.h"

#include <okx/signer.h>
#include <okx/clock.h>

namespace Okx = spp::App::Okx;

i32 main() {
    Test test{"empty"_v};

    Trace("ISO 8601 timestamp matches the documented format") {
        // 2020-12-08T09:08:57.715Z corresponds to 1607418537715 ms.
        auto ts = Okx::iso8601_ms(1607418537715LL);
        assert(ts == "2020-12-08T09:08:57.715Z"_v);
    }

    Trace("unix_sec_str truncates ms and emits decimal seconds") {
        auto ts = Okx::unix_sec_str(1607418537715LL);
        assert(ts == "1607418537"_v);
    }

    Trace("sign() reproduces the OpenSSL reference HMAC-SHA256+base64") {
        // Reference vector from the OKX docs' canonical signing example:
        //   secret = "22582BD0CFF14C41EDBF1AB98506286D"
        //   ts     = "2020-12-08T09:08:57.715Z"
        //   method = "POST"
        //   path   = "/api/v5/trade/order"
        //   body   = '{"instId":"BTC-USDT","tdMode":"cash","clOrdId":"b15","side":"buy","ordType":"limit","px":"2.15","sz":"2"}'
        // Expected signature (OpenSSL-computed):
        //   dI6rrL9rXW/HdaPKJ/6LC1OgvH4/PYju6R3CqixMTNQ=
        spp::String_View ts     = "2020-12-08T09:08:57.715Z"_v;
        spp::String_View method = "POST"_v;
        spp::String_View path   = "/api/v5/trade/order"_v;
        spp::String_View body   =
            R"({"instId":"BTC-USDT","tdMode":"cash","clOrdId":"b15","side":"buy","ordType":"limit","px":"2.15","sz":"2"})"_v;
        spp::String_View secret = "22582BD0CFF14C41EDBF1AB98506286D"_v;

        auto sig = Okx::sign<Mdefault>(ts, method, path, body, secret);
        assert(sig == "dI6rrL9rXW/HdaPKJ/6LC1OgvH4/PYju6R3CqixMTNQ="_v);
    }

    Trace("signed_request_full attaches all four OK-ACCESS-* headers") {
        Okx::Signer_Config cfg;
        cfg.api_key    = "test_key"_v;
        cfg.api_secret = "22582BD0CFF14C41EDBF1AB98506286D"_v;
        cfg.passphrase = "test_pass"_v;

        spp::String_View body =
            R"({"instId":"BTC-USDT","tdMode":"cash","clOrdId":"b15","side":"buy","ordType":"limit","px":"2.15","sz":"2"})"_v;

        auto sr = Okx::signed_request_full<Mdefault>(
            "POST"_v, "/api/v5/trade/order"_v, ""_v, body,
            Okx::k_mainnet_host, cfg, 1607418537715LL);

        // Path stays unchanged (no query) and host is the OKX mainnet.
        assert(sr.request.method == "POST"_v);
        assert(sr.request.host == "www.okx.com"_v);
        assert(sr.request.path == "/api/v5/trade/order"_v);
        // Body String_View must point into the owned buffer.
        assert(sr.request.body.length() == body.length());

        bool seen_key = false, seen_sign = false, seen_ts = false,
             seen_pass = false, seen_ct = false, seen_sim = false;
        for(const auto& h : sr.request.headers) {
            if(h.name == "OK-ACCESS-KEY"_v) {
                seen_key = true;
                assert(h.value == "test_key"_v);
            }
            if(h.name == "OK-ACCESS-SIGN"_v) {
                seen_sign = true;
                assert(h.value == "dI6rrL9rXW/HdaPKJ/6LC1OgvH4/PYju6R3CqixMTNQ="_v);
            }
            if(h.name == "OK-ACCESS-TIMESTAMP"_v) {
                seen_ts = true;
                assert(h.value == "2020-12-08T09:08:57.715Z"_v);
            }
            if(h.name == "OK-ACCESS-PASSPHRASE"_v) {
                seen_pass = true;
                assert(h.value == "test_pass"_v);
            }
            if(h.name == "Content-Type"_v) seen_ct = true;
            if(h.name == "x-simulated-trading"_v) seen_sim = true;
        }
        assert(seen_key && seen_sign && seen_ts && seen_pass && seen_ct);
        // Simulated-trading flag was not set; the header must be absent.
        assert(!seen_sim);
    }

    Trace("simulated_trading=true adds the x-simulated-trading header") {
        Okx::Signer_Config cfg;
        cfg.api_key    = "k"_v;
        cfg.api_secret = "s"_v;
        cfg.passphrase = "p"_v;
        cfg.simulated_trading = true;

        auto sr = Okx::signed_request_full<Mdefault>(
            "GET"_v, "/api/v5/account/balance"_v, ""_v, ""_v,
            Okx::k_mainnet_host, cfg, 1700000000000LL);

        bool seen_sim = false;
        for(const auto& h : sr.request.headers) {
            if(h.name == "x-simulated-trading"_v) {
                seen_sim = true;
                assert(h.value == "1"_v);
            }
        }
        assert(seen_sim);
    }

    Trace("GET request appends query to path and uses path?query in prehash") {
        Okx::Signer_Config cfg;
        cfg.api_key    = "k"_v;
        cfg.api_secret = "s"_v;
        cfg.passphrase = "p"_v;

        auto sr = Okx::signed_request_full<Mdefault>(
            "GET"_v, "/api/v5/market/candles"_v,
            "instId=BTC-USDT&bar=1m"_v, ""_v,
            Okx::k_mainnet_host, cfg, 1700000000000LL);

        // path member must include the query.
        assert(sr.request.path ==
               "/api/v5/market/candles?instId=BTC-USDT&bar=1m"_v);

        // Reference signature recomputed for this exact prehash.
        spp::String_View ts = "2023-11-14T22:13:20.000Z"_v;
        auto expected = Okx::sign<Mdefault>(
            ts, "GET"_v,
            "/api/v5/market/candles?instId=BTC-USDT&bar=1m"_v,
            ""_v, "s"_v);
        for(const auto& h : sr.request.headers) {
            if(h.name == "OK-ACCESS-SIGN"_v) {
                assert(h.value == expected.view());
            }
        }
    }

    return 0;
}
