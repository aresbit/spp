#include "test.h"

#include <spp/core/opt.h>
#include <spp/quant/strategy/strategy_base.h>
#include <spp/quant/data/ohlcv_data.h>

namespace QS = spp::quant::strategy;
namespace QD = spp::quant::data;

// CRTP probe that records how often the live loop drove each hook.
struct Live_Probe : QS::Strategy_Base<Live_Probe, Mdefault> {
    spp::u64 bars = 0;
    spp::u64 risk_calls = 0;
    spp::u64 stop_after = 0;  // 0 = never self-stop
    void on_bar(const QD::Bar&) noexcept { bars++; }
    void risk_check() noexcept {
        risk_calls++;
        if (stop_after != 0 && bars >= stop_after) stop();
    }
};

static QD::Symbol_Bar mk_bar(spp::i64 ms, f64 px) noexcept {
    QD::Symbol_Bar sb;
    sb.symbol   = "BTC-USDT"_v.string<Mdefault>();
    sb.bar.time = QD::Timestamp::from_unix_ms(ms);
    sb.bar.open = QD::f64_to_price(px);
    sb.bar.high = QD::f64_to_price(px);
    sb.bar.low  = QD::f64_to_price(px);
    sb.bar.close = QD::f64_to_price(px);
    sb.bar.volume = 1.0;
    return sb;
}

i32 main() {
    Test test{"empty"_v};

    Trace("run() drains a finite feed and stops cleanly") {
        Live_Probe strat;
        strat.running_mode = QS::Running_Mode::live;

        constexpr spp::u64 kBars = 5;
        spp::u64 idx = 0;
        auto next = [&]() -> spp::Opt<QD::Symbol_Bar> {
            if (idx >= kBars) return {};  // feed drained
            spp::i64 ms = 1700000000000LL + static_cast<spp::i64>(idx) * 60000;
            f64 px = 42000.0 + static_cast<f64>(idx);
            idx++;
            return spp::Opt<QD::Symbol_Bar>{mk_bar(ms, px)};
        };

        spp::u64 processed = strat.run(next);
        assert(processed == kBars);
        assert(strat.bars == kBars);
        assert(strat.risk_calls == kBars);
        assert(!strat.is_running());
    }

    Trace("stop() from a hook breaks an unbounded feed early") {
        Live_Probe strat;
        strat.running_mode = QS::Running_Mode::live;
        strat.stop_after = 3;  // self-stop once 3 bars are seen

        spp::u64 idx = 0;
        auto next = [&]() -> spp::Opt<QD::Symbol_Bar> {
            // Never returns empty — the strategy must break the loop itself.
            spp::i64 ms = 1700000000000LL + static_cast<spp::i64>(idx) * 60000;
            f64 px = 42000.0 + static_cast<f64>(idx);
            idx++;
            return spp::Opt<QD::Symbol_Bar>{mk_bar(ms, px)};
        };

        spp::u64 processed = strat.run(next);
        assert(processed == 3);
        assert(strat.bars == 3);
        assert(!strat.is_running());
    }

    return 0;
}
