#include "test.h"

#include <spp/io/stream.h>
#include <okx/api.h>

namespace Okx = spp::App::Okx;

// Helper (copy of binance test helper) — assembles an HTTP/1.1 200 OK
// response with the right Content-Length and injects it onto the wire.
static void inject_response(spp::Net::Memory_Stream& wire, spp::u32 status,
                            spp::String_View status_text,
                            spp::String_View extra_headers,
                            spp::String_View body) noexcept {
    spp::Vec<u8, Mdefault> out;
    auto put_sv = [&out](spp::String_View sv) {
        for(spp::u64 i = 0; i < sv.length(); i++) out.push(sv[i]);
    };
    auto put_lit = [&out](const char* s) {
        while(*s) out.push(static_cast<spp::u8>(*s++));
    };
    put_lit("HTTP/1.1 ");
    {
        u8 buf[16];
        i32 n = Libc::snprintf(buf, sizeof(buf), "%u", status);
        for(i32 i = 0; i < n; i++) out.push(buf[i]);
    }
    put_lit(" ");
    put_sv(status_text);
    put_lit("\r\n");
    put_sv(extra_headers);
    put_lit("Content-Length: ");
    {
        u8 buf[32];
        i32 n = Libc::snprintf(buf, sizeof(buf), "%lu",
                               static_cast<unsigned long>(body.length()));
        for(i32 i = 0; i < n; i++) out.push(buf[i]);
    }
    put_lit("\r\n\r\n");
    put_sv(body);
    wire.inject(out.slice());
}

i32 main() {
    Test test{"empty"_v};

    Trace("fetch_server_time round-trips the envelope shape") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 200, "OK"_v,
                        "Content-Type: application/json\r\n"_v,
                        R"({"code":"0","msg":"","data":[{"ts":"1700000000000"}]})"_v);

        Okx::Rate_Limiter rl;
        auto r = Okx::fetch_server_time(wire, Okx::k_mainnet_host, rl, 0);
        assert(r.ok());
        assert(r.unwrap().code == "0"_v);
        assert(r.unwrap().data.length() == 1);
        assert(r.unwrap().data[0].ts == "1700000000000"_v);
    }

    Trace("place_order body serializes optional fields conditionally") {
        // LIMIT order: should include px and sz; clOrdId omitted when empty.
        Okx::Order_Request req;
        req.instId  = "BTC-USDT"_v;
        req.tdMode  = Okx::Td_Mode::cash;
        req.side    = Okx::Side::buy;
        req.ordType = Okx::Ord_Type::limit;
        req.sz      = "0.001"_v;
        req.px      = "42000.0"_v;
        auto body = Okx::detail::build_order_body(req);
        assert(body ==
            R"({"instId":"BTC-USDT","tdMode":"cash","side":"buy","ordType":"limit","sz":"0.001","px":"42000.0"})"_v);

        // MARKET order with explicit clOrdId: px omitted, clOrdId present.
        Okx::Order_Request req2;
        req2.instId  = "BTC-USDT"_v;
        req2.side    = Okx::Side::sell;
        req2.ordType = Okx::Ord_Type::market;
        req2.sz      = "0.001"_v;
        req2.clOrdId = "myorder001"_v;
        auto body2 = Okx::detail::build_order_body(req2);
        assert(body2 ==
            R"({"instId":"BTC-USDT","tdMode":"cash","side":"sell","ordType":"market","sz":"0.001","clOrdId":"myorder001"})"_v);
    }

    Trace("place_order signs the body and parses the envelope") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 200, "OK"_v,
                        "Content-Type: application/json\r\n"_v,
                        R"({"code":"0","msg":"",)"
                        R"("data":[{"clOrdId":"","ordId":"312269865356374016",)"
                        R"("tag":"","sCode":"0","sMsg":""}]})"_v);

        Okx::Signer_Config cfg;
        cfg.api_key    = "k"_v;
        cfg.api_secret = "22582BD0CFF14C41EDBF1AB98506286D"_v;
        cfg.passphrase = "p"_v;

        Okx::Rate_Limiter rl;
        Okx::Order_Request req;
        req.instId  = "BTC-USDT"_v;
        req.side    = Okx::Side::buy;
        req.ordType = Okx::Ord_Type::limit;
        req.sz      = "0.001"_v;
        req.px      = "42000.0"_v;

        auto r = Okx::place_order(wire, Okx::k_mainnet_host, cfg, rl,
                                   1700000000000LL, req);
        assert(r.ok());
        assert(r.unwrap().code == "0"_v);
        assert(r.unwrap().data.length() == 1);
        assert(r.unwrap().data[0].ordId == "312269865356374016"_v);
        assert(r.unwrap().data[0].sCode == "0"_v);

        // Verify the request was POST /api/v5/trade/order and carried
        // all four OK-ACCESS-* headers.
        auto sent = wire.sent();
        spp::String_View req_text{sent.data(), sent.length()};
        assert(req_text.sub(0, 5) == "POST "_v);
        auto contains = [](spp::String_View hay, spp::String_View needle) {
            if(needle.length() > hay.length()) return false;
            for(spp::u64 i = 0; i + needle.length() <= hay.length(); i++) {
                if(hay.sub(i, i + needle.length()) == needle) return true;
            }
            return false;
        };
        assert(contains(req_text, "/api/v5/trade/order"_v));
        assert(contains(req_text, "OK-ACCESS-SIGN: "_v));
        assert(contains(req_text, "OK-ACCESS-KEY: "_v));
        assert(contains(req_text, "OK-ACCESS-TIMESTAMP: "_v));
        assert(contains(req_text, "OK-ACCESS-PASSPHRASE: "_v));
    }

    Trace("fetch_balance attaches signed GET headers, parses Balance_Resp") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 200, "OK"_v,
                        "Content-Type: application/json\r\n"_v,
                        R"({"code":"0","msg":"","data":[{"uTime":"1700000000000",)"
                        R"("totalEq":"10000.00","isoEq":"0",)"
                        R"("details":[{"ccy":"USDT","eq":"10000.00",)"
                        R"("availBal":"9500.00","frozenBal":"500.00"}]}]})"_v);

        Okx::Signer_Config cfg;
        cfg.api_key = "k"_v; cfg.api_secret = "s"_v; cfg.passphrase = "p"_v;
        Okx::Rate_Limiter rl;
        auto r = Okx::fetch_balance(wire, Okx::k_mainnet_host, cfg, rl,
                                     1700000000000LL, "USDT"_v);
        assert(r.ok());
        assert(r.unwrap().code == "0"_v);
        assert(r.unwrap().data.length() == 1);
        assert(r.unwrap().data[0].totalEq == "10000.00"_v);
        assert(r.unwrap().data[0].details.length() == 1);
        assert(r.unwrap().data[0].details[0].ccy == "USDT"_v);
        assert(r.unwrap().data[0].details[0].availBal == "9500.00"_v);
    }

    Trace("parse_candles decodes the OKX array-of-arrays shape") {
        spp::String_View body =
            R"({"code":"0","msg":"","data":[)"
            R"(["1700000000000","42000.0","42500.0","41800.0","42200.0",)"
            R"("1.5","63300","63300","1"],)"
            R"(["1700000060000","42200.0","42400.0","42100.0","42300.0",)"
            R"("0.8","33840","33840","1"]]})"_v;

        auto r = Okx::parse_candles(body);
        assert(r.ok());
        auto& cs = r.unwrap();
        assert(cs.length() == 2);
        assert(cs[0].open_time == 1700000000000LL);
        assert(cs[0].open == "42000.0"_v);
        assert(cs[0].high == "42500.0"_v);
        assert(cs[0].close == "42200.0"_v);
        assert(cs[0].vol == "1.5"_v);
        assert(cs[0].confirm == 1);
        assert(cs[1].open_time == 1700000060000LL);
        assert(cs[1].close == "42300.0"_v);
    }

    Trace("cancel_order serializes ordId-targeted body and POSTs") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 200, "OK"_v,
                        "Content-Type: application/json\r\n"_v,
                        R"({"code":"0","msg":"","data":[{"clOrdId":"",)"
                        R"("ordId":"312269865356374016","sCode":"0","sMsg":""}]})"_v);

        Okx::Signer_Config cfg;
        cfg.api_key = "k"_v; cfg.api_secret = "s"_v; cfg.passphrase = "p"_v;
        Okx::Rate_Limiter rl;
        auto r = Okx::cancel_order(wire, Okx::k_mainnet_host, cfg, rl,
                                    1700000000000LL,
                                    "BTC-USDT"_v, "312269865356374016"_v);
        assert(r.ok());
        assert(r.unwrap().code == "0"_v);
        assert(r.unwrap().data[0].ordId == "312269865356374016"_v);

        // The outbound body contains both instId and ordId, no clOrdId.
        auto sent = wire.sent();
        spp::String_View req_text{sent.data(), sent.length()};
        auto contains = [](spp::String_View hay, spp::String_View needle) {
            if(needle.length() > hay.length()) return false;
            for(spp::u64 i = 0; i + needle.length() <= hay.length(); i++) {
                if(hay.sub(i, i + needle.length()) == needle) return true;
            }
            return false;
        };
        assert(contains(req_text, "\"instId\":\"BTC-USDT\""_v));
        assert(contains(req_text, "\"ordId\":\"312269865356374016\""_v));
        assert(!contains(req_text, "\"clOrdId\""_v));
    }

    return 0;
}
