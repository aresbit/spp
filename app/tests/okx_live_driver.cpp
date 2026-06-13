#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/websocket.h>
#include <spp/quant/strategy/strategy_base.h>

#include <okx/live_driver.h>
#include <okx/ws_session.h>

namespace Okx = spp::App::Okx;
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

struct Order_On_Bar_Close
    : QS::Strategy_Base<Order_On_Bar_Close, Mdefault> {
    spp::u64 bars_seen = 0;
    void on_bar(const QD::Bar& bar) noexcept {
        bars_seen++;
        static_cast<void>(send_order(QS::Order_Direction::buy,
                                      QS::Order_Offset::open_,
                                      bar.close, 0.001, "BTC-USDT"_v));
    }
};

i32 main() {
    Test test{"empty"_v};

    Trace("OKX live driver: WS trade frame → bar close → place_order REST") {
        spp::Net::Memory_Stream market_wire;
        spp::Net::Memory_Stream rest_wire;

        Okx::Market_Stream<spp::Net::Memory_Stream> mkt{market_wire};
        mkt.ws.handshake_done = true;

        Okx::Rate_Limiter limiter;
        Order_On_Bar_Close strat;
        strat.init_cash = 1000.0;
        strat.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1000.0};
        strat.running_mode = QS::Running_Mode::live;

        Okx::Live_Driver<Order_On_Bar_Close, spp::Net::Memory_Stream,
                          spp::Net::Memory_Stream>
            driver{mkt, rest_wire, limiter, strat};
        driver.signer.api_key    = "k"_v;
        driver.signer.api_secret = "s"_v;
        driver.signer.passphrase = "p"_v;
        driver.register_instrument("BTC-USDT"_v, 60000);

        // place_order response (OKX envelope) for the order the strategy
        // will queue on the first bar close.
        inject_rest_ok(rest_wire,
            R"({"code":"0","msg":"","data":[)"
            R"({"clOrdId":"","ordId":"312269865356374016",)"
            R"("tag":"","sCode":"0","sMsg":""}]})"_v);

        constexpr spp::i64 B0 = 60000000000LL;
        constexpr spp::i64 B1 = B0 + 60000LL;

        // Frame 1: two trades inside B0.
        auto frame1 = spp::String<Mdefault>{};
        {
            char buf[512];
            i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
                R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
                R"({"instId":"BTC-USDT","px":"42000.0","sz":"0.5",)"
                R"("side":"buy","ts":"%lld"},)"
                R"({"instId":"BTC-USDT","px":"42100.0","sz":"0.3",)"
                R"("side":"sell","ts":"%lld"}]})",
                (long long)(B0 + 10000), (long long)(B0 + 30000));
            frame1 = spp::String<Mdefault>((u64)n);
            frame1.set_length((u64)n);
            Libc::memcpy(frame1.data(), buf, (u64)n);
        }
        // Frame 2: one trade in B1 → closes B0's bar.
        auto frame2 = spp::String<Mdefault>{};
        {
            char buf[256];
            i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
                R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
                R"({"instId":"BTC-USDT","px":"42050.0","sz":"0.1",)"
                R"("side":"buy","ts":"%lld"}]})",
                (long long)(B1 + 1000));
            frame2 = spp::String<Mdefault>((u64)n);
            frame2.set_length((u64)n);
            Libc::memcpy(frame2.data(), buf, (u64)n);
        }

        inject_frame(market_wire, frame1.view());
        inject_frame(market_wire, frame2.view());

        assert(driver.pump_market_once(0).ok());  // frame1 — two trades into B0
        assert(driver.bars_closed == 0);          // no boundary yet
        assert(driver.pump_market_once(0).ok());  // frame2 — closes B0
        assert(driver.bars_closed == 1);
        assert(strat.bars_seen == 1);
        assert(driver.orders_dispatched == 1);
        assert(driver.dispatch_failures == 0);
        // ordId stored from response.
        spp::String_View local_id = strat.acc.orders[0].order_id.view();
        assert(driver.exchange_id_for(local_id) == "312269865356374016"_v);

        // The REST request should be a SIGNED POST /api/v5/trade/order.
        auto sent = rest_wire.sent();
        spp::String_View req{sent.data(), sent.length()};
        auto contains = [](spp::String_View hay, spp::String_View needle) {
            if(needle.length() > hay.length()) return false;
            for(spp::u64 i = 0; i + needle.length() <= hay.length(); i++) {
                if(hay.sub(i, i + needle.length()) == needle) return true;
            }
            return false;
        };
        assert(req.sub(0, 5) == "POST "_v);
        assert(contains(req, "/api/v5/trade/order"_v));
        assert(contains(req, "OK-ACCESS-SIGN: "_v));
        assert(contains(req, "OK-ACCESS-TIMESTAMP: "_v));
        // The body carries the OKX field shape, not Binance's.
        assert(contains(req, "\"instId\":\"BTC-USDT\""_v));
        assert(contains(req, "\"tdMode\":\"cash\""_v));
        assert(contains(req, "\"side\":\"buy\""_v));
        // clOrdId must be OKX-legal: alphanumeric only.  The account
        // layer's local id is "ORD_<n>" (underscore), which OKX rejects;
        // the driver must strip it to "ORD<n>" on the wire.
        assert(contains(req, "\"clOrdId\":\"ORD1\""_v));
        assert(!contains(req, "ORD_1"_v));
    }

    Trace("pump_market_session_once: routes trades through Ws_Session "
          "and swallows pong frames") {
        // Stub connector that pretends TLS is always up.
        struct Stub_Connector {
            bool is_connected = true;
            struct Tls_View { bool* p; bool handshake_done() const noexcept { return *p; } } tls{&is_connected};
            [[nodiscard]] spp::Result<spp::i64, spp::String_View>
            try_connect(spp::i64) noexcept {
                return spp::Result<spp::i64, spp::String_View>::ok(0);
            }
        } conn;

        spp::Net::Memory_Stream market_wire;
        spp::Net::Memory_Stream rest_wire;

        Okx::Market_Stream<spp::Net::Memory_Stream> mkt{market_wire};
        mkt.ws.handshake_done = true;

        Okx::Rate_Limiter limiter;
        Order_On_Bar_Close strat;
        strat.init_cash = 1000.0;
        strat.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1000.0};
        strat.running_mode = QS::Running_Mode::live;

        Okx::Live_Driver<Order_On_Bar_Close, spp::Net::Memory_Stream,
                          spp::Net::Memory_Stream>
            driver{mkt, rest_wire, limiter, strat};
        driver.signer.api_key    = "k"_v;
        driver.signer.api_secret = "s"_v;
        driver.signer.passphrase = "p"_v;
        driver.register_instrument("BTC-USDT"_v, 60000);

        // Build a Ws_Session around the same Memory_Stream-backed
        // market stream.  No reconnect is exercised here — only the
        // "happy path through recv_or_reconnect + pong filtering".
        auto replay = [](auto& /*holder*/, spp::i64) noexcept {
            return spp::Result<spp::u64, spp::String_View>::ok(0);
        };
        Okx::Ws_Session<Okx::Market_Stream<spp::Net::Memory_Stream>,
                         decltype(replay), Stub_Connector>
            mkt_session{conn, mkt, replay};
        mkt_session.ws_open_ = true;
        mkt_session.last_activity_at_ms_ = 0;

        // 1. A pong frame in flight — driver must drop it silently
        //    without trying to parse it as a trade.
        inject_frame(market_wire, "pong"_v);
        auto rpong = driver.pump_market_session_once(mkt_session, /*now_ms=*/0);
        assert(rpong.ok());
        assert(rpong.unwrap() == 0);
        assert(mkt_session.pongs_received == 1);
        assert(driver.bars_closed == 0);

        // 2. A real trades frame — must route through to the
        //    aggregator and (eventually) close a bar.  Use the same
        //    B0 / B1 schedule as the legacy test.
        constexpr spp::i64 B0 = 60000000000LL;
        constexpr spp::i64 B1 = B0 + 60000LL;
        inject_rest_ok(rest_wire,
            R"({"code":"0","msg":"","data":[)"
            R"({"clOrdId":"","ordId":"99","tag":"","sCode":"0","sMsg":""}]})"_v);

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
        auto t1 = build(R"({"arg":{"channel":"trades"},"data":[{"instId":"BTC-USDT","px":"42000.0","sz":"0.5","side":"buy","ts":)"_v,
                        B0 + 10000);
        auto t2 = build(R"({"arg":{"channel":"trades"},"data":[{"instId":"BTC-USDT","px":"42100.0","sz":"0.3","side":"buy","ts":)"_v,
                        B1 + 1000);
        inject_frame(market_wire, t1.view());
        inject_frame(market_wire, t2.view());

        assert(driver.pump_market_session_once(mkt_session, 0).ok());
        // t2 (in bucket B1) triggers close of B0 → strategy.x1 fires
        // → REST place_order dispatched.
        assert(driver.pump_market_session_once(mkt_session, 0).ok());
        assert(driver.bars_closed == 1);
        assert(driver.orders_dispatched == 1);
        assert(driver.exchange_id_for(strat.acc.orders[0].order_id.view())
               == "99"_v);
    }

    Trace("OKX closed loop: bar close → place_order → user-stream FILL "
          "→ reconciler updates position + cash") {
        spp::Net::Memory_Stream market_wire;
        spp::Net::Memory_Stream rest_wire;
        spp::Net::Memory_Stream user_wire;

        Okx::Market_Stream<spp::Net::Memory_Stream> mkt{market_wire};
        mkt.ws.handshake_done = true;

        Okx::Rate_Limiter limiter;
        Order_On_Bar_Close strat;
        strat.init_cash = 1000.0;
        strat.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1000.0};
        strat.running_mode = QS::Running_Mode::live;

        Okx::Live_Driver<Order_On_Bar_Close, spp::Net::Memory_Stream,
                          spp::Net::Memory_Stream>
            driver{mkt, rest_wire, limiter, strat};
        driver.signer.api_key    = "k"_v;
        driver.signer.api_secret = "s"_v;
        driver.signer.passphrase = "p"_v;
        driver.register_instrument("BTC-USDT"_v, 60000);

        // Wire the user-stream reconciler onto the SAME account the
        // strategy trades — this is what closes the loop.
        Okx::Position_Reconciler_Okx<Mdefault> recon{strat.acc};
        driver.attach_reconciler(recon);

        Okx::User_Stream<spp::Net::Memory_Stream> user{user_wire};
        user.ws.handshake_done = true;

        inject_rest_ok(rest_wire,
            R"({"code":"0","msg":"","data":[)"
            R"({"clOrdId":"","ordId":"777","tag":"","sCode":"0","sMsg":""}]})"_v);

        // --- 1. Market frames close a bar → strategy queues a buy order. ---
        constexpr spp::i64 B0 = 60000000000LL;
        constexpr spp::i64 B1 = B0 + 60000LL;
        auto build_trade = [](spp::String_View px, spp::String_View side,
                              spp::i64 ts) {
            char buf[256];
            i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
                R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
                R"({"instId":"BTC-USDT","px":"%.*s","sz":"0.5","side":"%.*s",)"
                R"("ts":"%lld"}]})",
                (i32)px.length(), px.data(),
                (i32)side.length(), side.data(),
                (long long)ts);
            spp::String<Mdefault> out((u64)n);
            out.set_length((u64)n);
            Libc::memcpy(out.data(), buf, (u64)n);
            return out;
        };
        auto t0 = build_trade("42100.0"_v, "buy"_v, B0 + 10000);
        auto t1 = build_trade("42050.0"_v, "buy"_v, B1 + 1000);
        inject_frame(market_wire, t0.view());
        inject_frame(market_wire, t1.view());

        assert(driver.pump_market_once(0).ok());  // t0 into B0
        assert(driver.pump_market_once(0).ok());  // t1 closes B0
        assert(driver.bars_closed == 1);
        assert(driver.orders_dispatched == 1);
        assert(strat.acc.orders.length() == 1);
        assert(driver.exchange_id_for(strat.acc.orders[0].order_id.view())
               == "777"_v);

        f64 cash_before_fill = strat.acc.balance;

        // --- 2. Exchange pushes a FILL on the user data stream. The driver
        //        routes it through pump_user_once → reconciler → account. ---
        inject_frame(user_wire,
            R"({"arg":{"channel":"orders","instType":"SPOT"},"data":[)"
            R"({"instId":"BTC-USDT","ordId":"777","clOrdId":"",)"
            R"("side":"buy","fillPx":"42100.0","fillSz":"0.001",)"
            R"("fillFee":"-0.0126","fillFeeCcy":"USDT",)"
            R"("state":"filled"}]})"_v);

        auto pu = driver.pump_user_once(user);
        assert(pu.ok());

        // --- 3. The loop is closed: position opened, cash debited. ---
        assert(recon.fills_applied == 1);
        assert(strat.acc.trades.length() == 1);
        auto pos = strat.acc.get_position("BTC-USDT"_v);
        assert(pos.ok());
        assert(pos->volume_long >= 0.001 - 1e-9);
        assert(strat.acc.balance < cash_before_fill);  // notional + fee debited
    }

    Trace("OKX rejection: HTTP 200 + sCode!=0 must NOT count as placed "
          "and must NOT store an exchange-id mapping") {
        spp::Net::Memory_Stream market_wire;
        spp::Net::Memory_Stream rest_wire;

        Okx::Market_Stream<spp::Net::Memory_Stream> mkt{market_wire};
        mkt.ws.handshake_done = true;

        Okx::Rate_Limiter limiter;
        Order_On_Bar_Close strat;
        strat.init_cash = 1000.0;
        strat.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1000.0};
        strat.running_mode = QS::Running_Mode::live;

        Okx::Live_Driver<Order_On_Bar_Close, spp::Net::Memory_Stream,
                          spp::Net::Memory_Stream>
            driver{mkt, rest_wire, limiter, strat};
        driver.signer.api_key    = "k"_v;
        driver.signer.api_secret = "s"_v;
        driver.signer.passphrase = "p"_v;
        driver.register_instrument("BTC-USDT"_v, 60000);

        // OKX rejects the order at the order level: HTTP 200, top-level
        // code "1", per-order sCode "51008", EMPTY ordId.
        inject_rest_ok(rest_wire,
            R"({"code":"1","msg":"","data":[)"
            R"({"clOrdId":"","ordId":"","tag":"","sCode":"51008",)"
            R"("sMsg":"Order failed. Insufficient balance"}]})"_v);

        constexpr spp::i64 B0 = 60000000000LL;
        constexpr spp::i64 B1 = B0 + 60000LL;
        auto build_trade = [](spp::String_View px, spp::i64 ts) {
            char buf[256];
            i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
                R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
                R"({"instId":"BTC-USDT","px":"%.*s","sz":"0.5","side":"buy",)"
                R"("ts":"%lld"}]})",
                (i32)px.length(), px.data(), (long long)ts);
            spp::String<Mdefault> out((u64)n);
            out.set_length((u64)n);
            Libc::memcpy(out.data(), buf, (u64)n);
            return out;
        };
        auto t0 = build_trade("42100.0"_v, B0 + 10000);
        auto t1 = build_trade("42050.0"_v, B1 + 1000);
        inject_frame(market_wire, t0.view());
        inject_frame(market_wire, t1.view());

        assert(driver.pump_market_once(0).ok());  // t0 into B0
        assert(driver.pump_market_once(0).ok());  // t1 closes B0 → dispatch
        assert(driver.bars_closed == 1);
        // The order was rejected: NOT counted as dispatched, counted as a
        // failure, and NO phantom exchange-id mapping stored.
        assert(driver.orders_dispatched == 0);
        assert(driver.dispatch_failures == 1);
        assert(driver.exchange_id_for(strat.acc.orders[0].order_id.view())
               == ""_v);
        assert(driver.last_dispatch_err_ == "order_rejected"_v);
        assert(driver.last_dispatch_scode_.view() == "51008"_v);
    }

    return 0;
}
