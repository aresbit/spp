#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/websocket.h>

#include <okx/market_stream.h>

namespace Okx = spp::App::Okx;
namespace QD  = spp::quant::data;
namespace Ws  = spp::Protocol::Websocket;

i32 main() {
    Test test{"empty"_v};

    Trace("subscribe builds the OKX op/args/channel JSON shape") {
        spp::Net::Memory_Stream wire;
        Okx::Market_Stream<spp::Net::Memory_Stream> mkt{wire};
        mkt.ws.handshake_done = true;  // skip handshake; tested elsewhere

        Okx::Subscription subs[2] = {
            {"trades"_v, "BTC-USDT"_v},
            {"candle1m"_v, "ETH-USDT"_v},
        };
        auto r = mkt.subscribe(spp::Slice<const Okx::Subscription>{subs, 2});
        assert(r.ok());

        // Decode the masked client-side WS frame and verify the payload
        // matches the OKX subscribe JSON shape.
        auto sent = wire.sent();
        assert(sent.length() > 6);
        assert(sent[0] == 0x81); // FIN | text
        // Copy into a mutable buffer so decode() can unmask in place.
        spp::Vec<u8, Mdefault> mut;
        for(spp::u64 i = 0; i < sent.length(); i++) mut.push(sent[i]);
        auto dec = Ws::decode(mut.slice());
        assert(dec.ok());
        spp::String_View payload{dec.unwrap().frame.payload.data(),
                                 dec.unwrap().frame.payload.length()};
        assert(payload ==
            R"({"op":"subscribe","args":[)"
            R"({"channel":"trades","instId":"BTC-USDT"},)"
            R"({"channel":"candle1m","instId":"ETH-USDT"}]})"_v);
    }

    Trace("next_trade walks all trade entries in a batched frame") {
        // OKX may batch several trades in one frame.  Verify we extract
        // each one in order.
        spp::String_View body =
            R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
            R"({"instId":"BTC-USDT","tradeId":"1","px":"42000.0","sz":"0.5",)"
            R"("side":"buy","ts":"1700000000000"},)"
            R"({"instId":"BTC-USDT","tradeId":"2","px":"42100.0","sz":"0.3",)"
            R"("side":"sell","ts":"1700000010000"})"
            R"(]})"_v;

        spp::u64 cursor = 0;
        Okx::Okx_Trade t;
        assert(Okx::next_trade(body, cursor, t));
        assert(t.instId == "BTC-USDT"_v);
        assert(t.px == "42000.0"_v);
        assert(t.sz == "0.5"_v);
        assert(t.side == "buy"_v);
        assert(t.ts == "1700000000000"_v);

        assert(Okx::next_trade(body, cursor, t));
        assert(t.px == "42100.0"_v);
        assert(t.ts == "1700000010000"_v);

        // No more trades — cursor lands on the closing `]`.
        assert(!Okx::next_trade(body, cursor, t));
    }

    Trace("feed_aggregator funnels OKX trades through a Bar_Aggregator") {
        // Drive a Bar_Aggregator from an OKX trades frame and verify the
        // bar closes at the bucket boundary.
        spp::Vec<QD::Symbol_Bar, Mdefault> closed;
        auto on_bar = [&closed](const QD::Symbol_Bar& sb) noexcept {
            QD::Symbol_Bar copy;
            copy.symbol = sb.symbol.clone();
            copy.bar = sb.bar;
            closed.push(spp::move(copy));
        };

        spp::App::Binance::Bar_Aggregator agg{"BTC-USDT"_v, 60000, on_bar};

        // Two trades inside bucket B0 (60_000_000_000 ms), then one in
        // bucket B1 → exactly one bar should close.
        spp::String_View frame1 =
            R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
            R"({"instId":"BTC-USDT","px":"42000.0","sz":"0.5",)"
            R"("side":"buy","ts":"60000010000"},)"
            R"({"instId":"BTC-USDT","px":"42200.0","sz":"0.3",)"
            R"("side":"sell","ts":"60000030000"}]})"_v;
        Okx::feed_aggregator(frame1, agg);
        assert(closed.length() == 0);

        spp::String_View frame2 =
            R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[)"
            R"({"instId":"BTC-USDT","px":"42100.0","sz":"0.1",)"
            R"("side":"buy","ts":"60000061000"}]})"_v;
        Okx::feed_aggregator(frame2, agg);
        assert(closed.length() == 1);

        auto& sb = closed[0];
        assert(sb.symbol == "BTC-USDT"_v);
        f64 close_px = static_cast<f64>(sb.bar.close.raw()) /
                       static_cast<f64>(spp::Decimal<8>::factor());
        // Last trade in the bucket was at 42200.
        assert(close_px > 42199.9 && close_px < 42200.1);
        // Volume 0.5 + 0.3 = 0.8.
        assert(sb.bar.volume > 0.79 && sb.bar.volume < 0.81);
    }

    return 0;
}
