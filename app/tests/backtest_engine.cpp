#include "test.h"

#include <spp/quant/backtest/sim_engine.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/strategy/strategy_base.h>

namespace QB = spp::quant::backtest;
namespace QD = spp::quant::data;
namespace QS = spp::quant::strategy;

static QD::Symbol_Bar make_sb(spp::String_View sym, spp::i64 t_ms,
                              f64 o, f64 h, f64 l, f64 c, f64 vol) noexcept {
    QD::Symbol_Bar sb;
    sb.symbol = sym.template string<Mdefault>();
    sb.bar.time = spp::Deterministic_Time::from_unix_ms(t_ms);
    sb.bar.open  = QD::f64_to_price(o);
    sb.bar.high  = QD::f64_to_price(h);
    sb.bar.low   = QD::f64_to_price(l);
    sb.bar.close = QD::f64_to_price(c);
    sb.bar.volume = vol;
    return sb;
}

struct Noop_Strategy : QS::Strategy_Base<Noop_Strategy, Mdefault> {};

i32 main() {
    Test test{"empty"_v};

    Trace("group_by_time bundles bars sharing a timestamp, sorts ascending") {
        QD::Ohlcv_Data<Mdefault> data;
        // Out-of-order ingest with two symbols and three distinct
        // timestamps. group_by_time must sort & bundle correctly.
        data.bars.push(make_sb("BTCUSDT"_v, 200, 1, 1, 1, 1, 0.1));
        data.bars.push(make_sb("ETHUSDT"_v, 100, 1, 1, 1, 1, 0.1));
        data.bars.push(make_sb("BTCUSDT"_v, 100, 1, 1, 1, 1, 0.1));
        data.bars.push(make_sb("ETHUSDT"_v, 200, 1, 1, 1, 1, 0.1));
        data.bars.push(make_sb("BTCUSDT"_v, 300, 1, 1, 1, 1, 0.1));

        auto groups = data.group_by_time();
        assert(groups.length() == 3);
        assert(groups[0].first.unix_ms() == 100);
        assert(groups[1].first.unix_ms() == 200);
        assert(groups[2].first.unix_ms() == 300);
        assert(groups[0].second.length() == 2);  // BTC + ETH
        assert(groups[1].second.length() == 2);
        assert(groups[2].second.length() == 1);
    }

    Trace("mean_volume per symbol averages just that symbol's bars") {
        QD::Ohlcv_Data<Mdefault> data;
        data.bars.push(make_sb("BTCUSDT"_v, 100, 1, 1, 1, 1, 10.0));
        data.bars.push(make_sb("BTCUSDT"_v, 200, 1, 1, 1, 1, 20.0));
        data.bars.push(make_sb("ETHUSDT"_v, 100, 1, 1, 1, 1, 1000.0));

        // BTC: (10 + 20) / 2 = 15
        f64 btc = data.mean_volume("BTCUSDT"_v);
        assert(btc > 14.99 && btc < 15.01);
        // ETH: 1000 / 1 = 1000
        f64 eth = data.mean_volume("ETHUSDT"_v);
        assert(eth > 999.99 && eth < 1000.01);
        // Missing: 0
        assert(data.mean_volume("XRPUSDT"_v) == 0.0);
    }

    Trace("Backtest_Engine.run() walks all groups end-to-end without crashing") {
        // Tiny scenario: 4 bars over 4 minutes, one symbol, noop strategy.
        // The engine doesn't need to trade — we're verifying the event
        // loop reaches the end and produces an equity curve of the right
        // shape.
        QD::Ohlcv_Data<Mdefault> data;
        constexpr spp::i64 ms = 60000;
        data.bars.push(make_sb("BTCUSDT"_v, 0 * ms, 100, 110, 95, 105, 100));
        data.bars.push(make_sb("BTCUSDT"_v, 1 * ms, 105, 115, 100, 110, 120));
        data.bars.push(make_sb("BTCUSDT"_v, 2 * ms, 110, 112, 108, 109, 80));
        data.bars.push(make_sb("BTCUSDT"_v, 3 * ms, 109, 115, 107, 114, 90));

        QB::Backtest_Config cfg;
        cfg.init_cash = 1000.0;
        cfg.portfolio_size = 1;
        cfg.max_weight_per_position = 1.0;

        QB::Backtest_Engine<Noop_Strategy, Mdefault> eng{spp::move(cfg),
                                                          Noop_Strategy{}};
        eng.strategy.codes.push("BTCUSDT"_v.string<Mdefault>());
        // Crypto markets are 24/7 — bypass the stock-hours filter.
        eng.strategy.market_type = QS::Market_Type::crypto;

        auto result = eng.run(data);
        // 4 group iterations + the initial snapshot before the loop = 5.
        assert(result.equity_curve.length() == 5);
        // No trades from a noop strategy.
        assert(eng.total_buys == 0);
        assert(eng.total_sells == 0);
        // _volume_cache populated from the data.
        f64 cached = eng._volume_cache.get("BTCUSDT"_v.string<Mdefault>());
        assert(cached > 0.0);
        // Final equity equals initial cash (no trades happened).
        assert(result.final_equity == 1000.0);
    }

    return 0;
}
