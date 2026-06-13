#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/websocket.h>
#include <spp/quant/strategy/strategy_base.h>

#include <binance/live_driver.h>
#include <binance/ws_client.h>

namespace Bnc = spp::App::Binance;
namespace Ws  = spp::Protocol::Websocket;
namespace QS  = spp::quant::strategy;
namespace QD  = spp::quant::data;

static void inject_frame(spp::Net::Memory_Stream& wire, spp::String_View payload) noexcept {
    spp::Vec<u8, Mdefault> buf;
    Ws::Frame f;
    f.fin = true;
    f.op = Ws::Opcode::text;
    f.payload = spp::Slice<const spp::u8>{payload.data(), payload.length()};
    Ws::encode(buf, f, false);
    wire.inject(buf.slice());
}

static void inject_rest_ok(spp::Net::Memory_Stream& rest,
                           spp::String_View body) noexcept {
    spp::Vec<u8, Mdefault> out;
    auto push_lit = [&out](const char* s) {
        while(*s) out.push(static_cast<spp::u8>(*s++));
    };
    auto push_sv = [&out](spp::String_View sv) {
        for(spp::u64 i = 0; i < sv.length(); i++) out.push(sv[i]);
    };
    push_lit("HTTP/1.1 200 OK\r\n");
    push_lit("Content-Type: application/json\r\n");
    push_lit("X-MBX-USED-WEIGHT-1M: 1\r\n");
    push_lit("Content-Length: ");
    {
        u8 buf[32];
        i32 n = Libc::snprintf(buf, sizeof(buf), "%lu",
                               static_cast<unsigned long>(body.length()));
        for(i32 i = 0; i < n; i++) out.push(buf[i]);
    }
    push_lit("\r\n\r\n");
    push_sv(body);
    rest.inject(out.slice());
}

struct One_Shot_Buyer : QS::Strategy_Base<One_Shot_Buyer, Mdefault> {
    bool placed = false;
    bool requested_cancel = false;
    spp::u64 cancel_target_idx = 0;

    void on_bar(const QD::Bar& bar) noexcept {
        if(!placed) {
            static_cast<void>(send_order(QS::Order_Direction::buy,
                                          QS::Order_Offset::open_,
                                          bar.close, 0.001, "BTCUSDT"_v));
            placed = true;
        } else if(!requested_cancel && acc.orders.length() > 0) {
            // Strategy decides to cancel the previously-placed order by
            // flipping its status to 2 (cancelled). The live driver picks
            // this up and dispatches a DELETE /api/v3/order.
            cancel_target_idx = 0;
            acc.orders[cancel_target_idx].status = 2;
            requested_cancel = true;
        }
    }
};

i32 main() {
    Test test{"empty"_v};

    Trace("place_order response orderId stored; cancel uses exchange ID") {
        spp::Net::Memory_Stream market;
        spp::Net::Memory_Stream rest;

        Bnc::Market_Stream<spp::Net::Memory_Stream> mkt{market};
        mkt.ws.handshake_done = true;

        Bnc::Rate_Limiter limiter;
        Bnc::Time_Sync time;
        One_Shot_Buyer strat;
        strat.init_cash = 1000.0;
        strat.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1000.0};
        strat.running_mode = QS::Running_Mode::live;

        Bnc::Live_Driver<One_Shot_Buyer, spp::Net::Memory_Stream,
                          spp::Net::Memory_Stream>
            driver{mkt, rest, limiter, time, strat};
        driver.register_symbol("BTCUSDT"_v, 60000);

        // Only the place_order response is injected up front; the cancel
        // response is injected just before pump #3. Reason: Http::fetch
        // does chunked reads during header scan and may pull a few bytes
        // past Content-Length into a subsequent response. Production
        // uses one-connection-per-request so this never bites; the test
        // sidesteps it by serialising the injections.
        inject_rest_ok(rest,
            R"({"symbol":"BTCUSDT","orderId":7777,"orderListId":-1,)"
            R"("clientOrderId":"","transactTime":1700000000000,)"
            R"("price":"42000.00","origQty":"0.001","executedQty":"0",)"
            R"("cummulativeQuoteQty":"0","status":"NEW",)"
            R"("timeInForce":"GTC","type":"LIMIT","side":"BUY"})"_v);

        constexpr spp::i64 B0 = 60000000000LL;
        constexpr spp::i64 B1 = B0 + 60000LL;
        constexpr spp::i64 B2 = B1 + 60000LL;

        auto build = [](spp::String_View prefix, spp::i64 ts) {
            char buf[256];
            i32 n = Libc::snprintf((u8*)buf, sizeof(buf), "%.*s%lld}",
                                    (i32)prefix.length(), prefix.data(),
                                    static_cast<long long>(ts));
            spp::String<Mdefault> out((u64)n);
            out.set_length((u64)n);
            Libc::memcpy(out.data(), buf, (u64)n);
            return out;
        };

        // Bar 1 close → strategy places order. Need a trade inside the
        // bucket then a trade after it.
        auto t1 = build(
            R"({"e":"aggTrade","s":"BTCUSDT","p":"42000.0","q":"0.5","T":)"_v,
            B0 + 10000);
        auto t2 = build(
            R"({"e":"aggTrade","s":"BTCUSDT","p":"42100.0","q":"0.3","T":)"_v,
            B1 + 1000);
        auto t3 = build(
            R"({"e":"aggTrade","s":"BTCUSDT","p":"42200.0","q":"0.1","T":)"_v,
            B2 + 1000);
        inject_frame(market, t1.view());
        inject_frame(market, t2.view());
        inject_frame(market, t3.view());

        assert(driver.pump_market_once(0).ok()); // ingest t1
        assert(driver.pump_market_once(0).ok()); // t2 closes B0 bar → place_order
        assert(driver.orders_dispatched == 1);
        // Exchange order ID stored from the response.
        spp::String_View local_id = strat.acc.orders[0].order_id.view();
        assert(driver.exchange_id_for(local_id) == 7777);

        inject_rest_ok(rest,
            R"({"symbol":"BTCUSDT","orderId":7777,"orderListId":-1,)"
            R"("clientOrderId":"","price":"42000.00","origQty":"0.001",)"
            R"("executedQty":"0","cummulativeQuoteQty":"0",)"
            R"("status":"CANCELED","timeInForce":"GTC","type":"LIMIT",)"
            R"("side":"BUY","time":1700000000000,"updateTime":1700000001000,)"
            R"("isWorking":false})"_v);

        assert(driver.pump_market_once(0).ok()); // t3 closes B1 bar → cancel
        assert(driver.cancels_dispatched == 1);
        assert(driver.cancel_failures == 0);

        // The DELETE request must include orderId=7777 in its query.
        auto rest_sent = rest.sent();
        spp::String_View rest_req{rest_sent.data(), rest_sent.length()};
        bool has_delete = false;
        for(spp::u64 i = 0; i + 7 < rest_req.length(); i++) {
            if(rest_req.sub(i, i + 7) == "DELETE "_v) { has_delete = true; break; }
        }
        assert(has_delete);
        bool has_order_id_param = false;
        for(spp::u64 i = 0; i + 13 < rest_req.length(); i++) {
            if(rest_req.sub(i, i + 13) == "orderId=7777"_v ||
               rest_req.sub(i, i + 13) == "orderId=7777&"_v) {
                has_order_id_param = true;
                break;
            }
        }
        assert(has_order_id_param);
    }

    return 0;
}
