#include "test.h"

#include <spp/io/stream.h>

#include <binance/api.h>

namespace Bnc = spp::App::Binance;

// Helper: assemble a complete HTTP/1.1 response with the correct
// Content-Length so test fixtures don't have to manually pre-count bytes.
static void inject_response(spp::Net::Memory_Stream& wire, spp::u32 status,
                            spp::String_View status_text, spp::String_View extra_headers,
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

    Trace("fetch_ticker_price end-to-end via Memory_Stream") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 200, "OK"_v,
                        "Content-Type: application/json\r\nX-MBX-USED-WEIGHT-1M: 4\r\n"_v,
                        "{\"symbol\":\"BTCUSDT\",\"price\":\"42500.12345678\"}"_v);

        Bnc::Rate_Limiter rl;
        auto res = Bnc::fetch_ticker_price(wire, "api.binance.com"_v, rl, 0, "BTCUSDT"_v);
        assert(res.ok());
        assert(res.unwrap().symbol == "BTCUSDT"_v);
        assert(res.unwrap().price == "42500.12345678"_v);

        // Rate limiter must have absorbed the X-MBX-USED-WEIGHT-1M echo.
        assert(rl.buckets[0].used == 4);

        // The request bytes that hit the wire must look like a proper HTTP/1.1
        // GET for the ticker endpoint.
        auto outgoing = wire.sent();
        spp::String_View req{outgoing.data(), outgoing.length()};
        assert(req.sub(0, 4) == "GET "_v);
        bool has_symbol = false;
        for(spp::u64 i = 0; i + 27 < req.length(); i++) {
            if(req.sub(i, i + 27) == "/api/v3/ticker/price?symbol"_v) {
                has_symbol = true;
                break;
            }
        }
        assert(has_symbol);
    }

    Trace("place_order builds a SIGNED request with all params + signature tail") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 200, "OK"_v, "X-MBX-USED-WEIGHT-1M: 6\r\n"_v,
                        "{\"symbol\":\"BTCUSDT\",\"orderId\":1,\"orderListId\":-1,"
                        "\"clientOrderId\":\"abc\",\"transactTime\":1700000000000,"
                        "\"price\":\"42000.00000000\",\"origQty\":\"0.01000000\","
                        "\"executedQty\":\"0.00000000\",\"cummulativeQuoteQty\":\"0.00000000\","
                        "\"status\":\"NEW\",\"timeInForce\":\"GTC\",\"type\":\"LIMIT\",\"side\":\"BUY\"}"_v);

        Bnc::Rate_Limiter rl;
        Bnc::Time_Sync ts;
        ts.skew_ms = 0;
        ts.samples = 1; // pretend we already synced so the call doesn't refresh

        Bnc::Order_Request req;
        req.symbol = "BTCUSDT"_v;
        req.side = Bnc::Side::BUY;
        req.type = Bnc::Order_Type::LIMIT;
        req.time_in_force = spp::Opt<Bnc::Time_In_Force>{Bnc::Time_In_Force::GTC};
        req.quantity = "0.01"_v;
        req.price = "42000.0"_v;

        auto res = Bnc::place_order(wire, "api.binance.com"_v, "KEY"_v, "SECRET"_v, rl, ts,
                                    1700000000000LL, req);
        assert(res.ok());
        assert(res.unwrap().orderId == 1);
        assert(res.unwrap().status == "NEW"_v);
        assert(rl.buckets[0].used == 6);

        // Inspect what we sent — the signed query must contain timestamp +
        // signature, and the request must carry X-MBX-APIKEY: KEY.
        auto outgoing = wire.sent();
        spp::String_View req_bytes{outgoing.data(), outgoing.length()};
        bool has_apikey = false;
        bool has_signature = false;
        bool has_post = false;
        for(spp::u64 i = 0; i + 18 < req_bytes.length(); i++) {
            if(req_bytes.sub(i, i + 18) == "X-MBX-APIKEY: KEY\r"_v) has_apikey = true;
            if(req_bytes.sub(i, i + 11) == "&signature="_v) has_signature = true;
        }
        if(req_bytes.length() >= 4 && req_bytes.sub(0, 4) == "POST"_v) has_post = true;
        assert(has_post);
        assert(has_apikey);
        assert(has_signature);
    }

    Trace("Rate-limiter cooldown blocks subsequent fetch") {
        spp::Net::Memory_Stream wire;
        inject_response(wire, 429, "Too Many Requests"_v, "Retry-After: 2\r\n"_v, ""_v);

        Bnc::Rate_Limiter rl;
        auto res = Bnc::fetch_ticker_price(wire, "api.binance.com"_v, rl, 0, "BTCUSDT"_v);
        // 429 surfaces as a Result error and installs a cooldown.
        assert(!res.ok());
        assert(rl.cooldown_until_ms > 0);
        assert(rl.wait_required_ms(1, 0) > 0);
    }

    return 0;
}
