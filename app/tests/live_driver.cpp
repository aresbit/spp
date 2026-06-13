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

// Inject one server frame (text or binary) onto `wire`.
static void inject_frame(spp::Net::Memory_Stream& wire, spp::String_View payload,
                         Ws::Opcode op = Ws::Opcode::text) noexcept {
    spp::Vec<u8, Mdefault> buf;
    Ws::Frame f;
    f.fin = true;
    f.op = op;
    f.payload = spp::Slice<const spp::u8>{payload.data(), payload.length()};
    Ws::encode(buf, f, false);
    wire.inject(buf.slice());
}

// Inject a 200 OK REST response for the next place_order call.
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

// Trivial CRTP strategy that submits one BUY at the close of every bar.
// Used to verify the driver's WS → x1() → order-dispatch path end-to-end.
struct Order_On_Every_Bar
    : QS::Strategy_Base<Order_On_Every_Bar, Mdefault> {

    spp::u64 bars_seen = 0;

    void on_bar(const QD::Bar& bar) noexcept {
        bars_seen++;
        // Queue a small LIMIT buy at the bar close. send_order in sim/live
        // mode places it on acc.orders with status==0 — the driver will
        // forward it to the exchange.
        static_cast<void>(send_order(QS::Order_Direction::buy,
                                      QS::Order_Offset::open_,
                                      bar.close, 0.001, "BTCUSDT"_v));
    }
};

i32 main() {
    Test test{"empty"_v};

    Trace("Live driver: bar close → strategy.x1 → REST place_order") {
        spp::Net::Memory_Stream market_wire;
        spp::Net::Memory_Stream rest_wire;

        // 1. Complete the market-stream WS handshake (we have to mint the
        //    handshake response after-the-fact so the client's nonce matches).
        Bnc::Market_Stream<spp::Net::Memory_Stream> mkt{market_wire};

        Bnc::Rate_Limiter limiter;
        Bnc::Time_Sync time;
        Order_On_Every_Bar strat;
        strat.init_cash = 1000.0;
        strat.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1000.0};
        strat.running_mode = QS::Running_Mode::live;

        Bnc::Live_Driver<Order_On_Every_Bar, spp::Net::Memory_Stream,
                          spp::Net::Memory_Stream>
            driver{mkt, rest_wire, limiter, time, strat};
        driver.register_symbol("BTCUSDT"_v, 60000);

        // Skip the WS handshake entirely — it's tested in ws_client.cpp.
        // The live driver only depends on the post-handshake recv_message
        // path, so we fast-forward Ws_Client into the open state.
        mkt.ws.handshake_done = true;

        // 2. Inject three aggTrades inside one minute and one in the next.
        constexpr spp::i64 B0 = 60000000000LL;
        constexpr spp::i64 B1 = B0 + 60000LL;

        // Pre-inject the REST response so the driver's place_order call
        // (fired when the bar closes) has bytes to read.
        inject_rest_ok(rest_wire,
            R"({"symbol":"BTCUSDT","orderId":1,"orderListId":-1,)"
            R"("clientOrderId":"","transactTime":1700000000000,)"
            R"("price":"42000.00","origQty":"0.001","executedQty":"0",)"
            R"("cummulativeQuoteQty":"0","status":"NEW",)"
            R"("timeInForce":"GTC","type":"LIMIT","side":"BUY"})"_v);

        auto trade1 = R"({"e":"aggTrade","s":"BTCUSDT","p":"42000.0","q":"0.5","T":)"_v;
        auto trade2 = R"({"e":"aggTrade","s":"BTCUSDT","p":"42100.0","q":"0.3","T":)"_v;
        auto trade3 = R"({"e":"aggTrade","s":"BTCUSDT","p":"42050.0","q":"0.2","T":)"_v;
        auto trade_next = R"({"e":"aggTrade","s":"BTCUSDT","p":"42200.0","q":"0.1","T":)"_v;

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

        auto t1 = build(trade1, B0 + 10000);
        auto t2 = build(trade2, B0 + 30000);
        auto t3 = build(trade3, B0 + 59000);
        auto tnext = build(trade_next, B1 + 1000);

        inject_frame(market_wire, t1.view());
        inject_frame(market_wire, t2.view());
        inject_frame(market_wire, t3.view());
        inject_frame(market_wire, tnext.view());

        // 3. Pump four messages. The fourth crosses the bucket boundary
        //    and triggers bar close → strategy.x1 → place_order.
        assert(driver.pump_market_once(0).ok());
        assert(driver.pump_market_once(0).ok());
        assert(driver.pump_market_once(0).ok());
        assert(driver.pump_market_once(0).ok());

        assert(driver.bars_closed == 1);
        assert(strat.bars_seen == 1);
        // Order dispatched (REST response was OK).
        assert(driver.orders_dispatched == 1);
        assert(driver.dispatch_failures == 0);

        // Verify the REST wire saw a SIGNED POST to /api/v3/order.
        auto rest_sent = rest_wire.sent();
        spp::String_View rest_req{rest_sent.data(), rest_sent.length()};
        assert(rest_req.sub(0, 5) == "POST "_v);
        bool has_order_path = false;
        for(spp::u64 i = 0; i + 14 < rest_req.length(); i++) {
            if(rest_req.sub(i, i + 14) == "/api/v3/order?"_v) {
                has_order_path = true;
                break;
            }
        }
        assert(has_order_path);
        // Signature is appended to the query.
        bool has_signature = false;
        for(spp::u64 i = 0; i + 11 < rest_req.length(); i++) {
            if(rest_req.sub(i, i + 11) == "&signature="_v) {
                has_signature = true;
                break;
            }
        }
        assert(has_signature);
    }

    return 0;
}
