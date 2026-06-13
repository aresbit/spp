#include "test.h"

#include <binance/bar_aggregator.h>
#include <spp/quant/data/ohlcv_data.h>

namespace Bnc = spp::App::Binance;
namespace QD  = spp::quant::data;

// Tiny captured-state callback so tests can inspect what the aggregator
// emitted without polluting the aggregator's API surface with a side
// channel.
struct Sink {
    spp::Vec<QD::Symbol_Bar, Mdefault> bars;
    void operator()(const QD::Symbol_Bar& sb) noexcept {
        QD::Symbol_Bar copy;
        copy.symbol = sb.symbol.clone();
        copy.bar = sb.bar;
        bars.push(spp::move(copy));
    }
};

// Build a minimal aggTrade JSON. We don't bother with optional fields the
// aggregator ignores (a, f, l, m, M) since their absence shouldn't change
// behaviour.
static spp::String<Mdefault>
make_trade(spp::String_View symbol, spp::String_View price,
           spp::String_View qty, spp::i64 trade_ms) noexcept {
    char buf[256];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"e\":\"aggTrade\",\"s\":\"%.*s\",\"p\":\"%.*s\",\"q\":\"%.*s\",\"T\":%lld}",
        (i32)symbol.length(), symbol.data(),
        (i32)price.length(),  price.data(),
        (i32)qty.length(),    qty.data(),
        static_cast<long long>(trade_ms));
    spp::String<Mdefault> out((u64)n);
    out.set_length((u64)n);
    Libc::memcpy(out.data(), buf, (u64)n);
    return out;
}

static f64 dec_to_f64(spp::Decimal<8> d) noexcept {
    return static_cast<f64>(d.raw()) /
           static_cast<f64>(spp::Decimal<8>::factor());
}

i32 main() {
    Test test{"empty"_v};

    Trace("aggTrade JSON field extraction") {
        spp::String_View body =
            R"({"e":"aggTrade","s":"BTCUSDT","p":"42000.50","q":"0.001","T":1700000000000})"_v;
        assert(Bnc::detail::json_field_(body, "s"_v) == "BTCUSDT"_v);
        assert(Bnc::detail::json_field_(body, "p"_v) == "42000.50"_v);
        assert(Bnc::detail::json_field_(body, "q"_v) == "0.001"_v);
        assert(Bnc::detail::json_field_(body, "T"_v) == "1700000000000"_v);
        // Missing key returns empty.
        assert(Bnc::detail::json_field_(body, "x"_v).length() == 0);
    }

    Trace("Decimal parser handles negatives and fractions") {
        assert(Bnc::detail::parse_decimal_("42000.5"_v) == 42000.5);
        assert(Bnc::detail::parse_decimal_("-12.25"_v) == -12.25);
        assert(Bnc::detail::parse_decimal_("0"_v) == 0.0);
        assert(Bnc::detail::parse_decimal_("0.00000001"_v) > 0.0);
    }

    // Clean bucket boundaries: B0 = 60_000_000_000 ms, B1 = B0 + 60_000.
    constexpr spp::i64 B0 = 60000000000LL;
    constexpr spp::i64 B1 = B0 + 60000LL;

    Trace("Single bucket: OHLC accumulates, no close fired yet") {
        Sink sink;
        Bnc::Bar_Aggregator<Sink&> agg{"BTCUSDT"_v, 60000, sink};

        auto t = make_trade("BTCUSDT"_v, "42000.00"_v, "1.0"_v, B0 + 10000);
        assert(agg.on_message(t.view()).ok());
        t = make_trade("BTCUSDT"_v, "42100.00"_v, "2.0"_v, B0 + 30000);
        assert(agg.on_message(t.view()).ok());
        t = make_trade("BTCUSDT"_v, "41950.00"_v, "0.5"_v, B0 + 59999);
        assert(agg.on_message(t.view()).ok());

        // All three trades fall in the same minute bucket — no close yet.
        assert(sink.bars.length() == 0);
        assert(agg.in_flight);
        assert(agg.cur_bucket_start_ms == B0);
    }

    Trace("Bucket rollover closes prior bar and exposes correct OHLCV") {
        Sink sink;
        Bnc::Bar_Aggregator<Sink&> agg{"BTCUSDT"_v, 60000, sink};

        // Three trades inside bucket B0.
        auto t = make_trade("BTCUSDT"_v, "42000.00"_v, "1.0"_v, B0 + 0);
        assert(agg.on_message(t.view()).ok());
        t = make_trade("BTCUSDT"_v, "42500.00"_v, "0.5"_v, B0 + 30000);
        assert(agg.on_message(t.view()).ok());
        t = make_trade("BTCUSDT"_v, "41800.00"_v, "0.25"_v, B0 + 45000);
        assert(agg.on_message(t.view()).ok());

        // A trade in bucket B1: prior bar must close.
        t = make_trade("BTCUSDT"_v, "42050.00"_v, "0.1"_v, B1 + 1000);
        assert(agg.on_message(t.view()).ok());

        assert(sink.bars.length() == 1);
        auto& sb = sink.bars[0];
        assert(sb.symbol == "BTCUSDT"_v);
        assert(sb.bar.time.unix_ms() == B0);
        assert(dec_to_f64(sb.bar.open)  == 42000.0);
        assert(dec_to_f64(sb.bar.high)  == 42500.0);
        assert(dec_to_f64(sb.bar.low)   == 41800.0);
        assert(dec_to_f64(sb.bar.close) == 41800.0);
        // Volume = 1.0 + 0.5 + 0.25 = 1.75
        assert(sb.bar.volume > 1.749 && sb.bar.volume < 1.751);
    }

    Trace("flush_if_due closes a stale in-flight bar even with no trade") {
        Sink sink;
        Bnc::Bar_Aggregator<Sink&> agg{"BTCUSDT"_v, 60000, sink};

        auto t = make_trade("BTCUSDT"_v, "42000.0"_v, "1.0"_v, B0 + 100);
        assert(agg.on_message(t.view()).ok());

        // now_ms still inside the bucket → no close.
        agg.flush_if_due(B1 - 1);
        assert(sink.bars.length() == 0);

        // Past the boundary → close.
        agg.flush_if_due(B1);
        assert(sink.bars.length() == 1);
        assert(!agg.in_flight);
    }

    Trace("Mismatched symbol is rejected; mixed-symbol bars are impossible") {
        Sink sink;
        Bnc::Bar_Aggregator<Sink&> agg{"BTCUSDT"_v, 60000, sink};

        auto t = make_trade("ETHUSDT"_v, "2000.0"_v, "1.0"_v, 1700000000000);
        auto r = agg.on_message(t.view());
        assert(!r.ok());
        assert(r.unwrap_err() == "aggTrade_symbol_mismatch"_v);
    }

    Trace("Missing mandatory field is reported, not silently dropped") {
        Sink sink;
        Bnc::Bar_Aggregator<Sink&> agg{"BTCUSDT"_v, 60000, sink};

        // No "T" field.
        spp::String_View bad =
            R"({"e":"aggTrade","s":"BTCUSDT","p":"42000","q":"1.0"})"_v;
        auto r = agg.on_message(bad);
        assert(!r.ok());
        assert(r.unwrap_err() == "aggTrade_missing_field"_v);
    }

    return 0;
}
