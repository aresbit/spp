#include "test.h"

#include <strategies/chan_live.h>

namespace CL = spp::App::Strategies;
namespace QD = spp::quant::data;
namespace QS = spp::quant::strategy;

// Build a Bar with the requested OHLC and timestamp.
static QD::Bar make_bar(i64 t_ms, f64 o, f64 h, f64 l, f64 c, f64 vol) noexcept {
    QD::Bar b;
    b.time = spp::Deterministic_Time::from_unix_ms(t_ms);
    b.open  = QD::f64_to_price(o);
    b.high  = QD::f64_to_price(h);
    b.low   = QD::f64_to_price(l);
    b.close = QD::f64_to_price(c);
    b.volume = vol;
    return b;
}

i32 main() {
    Test test{"empty"_v};

    Trace("Streaming detector bounds memory to 2*window+1 bars") {
        CL::Chan_Live_Config cfg;
        cfg.symbol = "BTCUSDT"_v;
        cfg.fractal_window = 3;

        CL::Chan_Live_Strategy s{cfg};
        s.init_cash = 1.0; // small to avoid any actual entry attempts
        s.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 1.0};
        // Use backtest mode so send_order also calls make_deal → the
        // position materialises immediately and the test can assert
        // on it without simulating a user-stream fill round-trip.
        s.running_mode = QS::Running_Mode::backtest;

        // Feed 200 flat bars — no fractals possible, but the detector's
        // internal window must stay bounded at 2*window+1 == 7.
        for(i64 i = 0; i < 200; i++) {
            s.on_bar(make_bar(i * 60000, 100, 100, 100, 100, 1.0));
        }
        assert(s.chan.bar_window.length() == 7);
        assert(s.chan.bars_pushed == 200);
        // Flat prices never yield a fractal.
        assert(s.chan.fractals.length() == 0);
    }

    Trace("Detects a bottom fractal and opens a long") {
        CL::Chan_Live_Config cfg;
        cfg.symbol = "BTCUSDT"_v;
        cfg.fractal_window = 2;        // smaller window for tight fixture
        cfg.max_history_bars = 50;
        cfg.entry_cash_fraction = 0.5;
        cfg.stop_buffer_pct = 0.0;

        CL::Chan_Live_Strategy s{cfg};
        s.init_cash = 10000.0;
        s.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 10000.0};
        // Use backtest mode so send_order also calls make_deal → the
        // position materialises immediately and the test can assert
        // on it without simulating a user-stream fill round-trip.
        s.running_mode = QS::Running_Mode::backtest;

        // Construct a "V" shape: prices drop then rebound, producing a
        // bottom fractal at the centre with window=2. Bars carry close
        // = high = low to keep detect_fractals deterministic.
        const f64 levels[] = {
            108, 106, 104, 102, 100,  // descend
             98,                       // local bottom (idx 5 in window)
            102, 104, 106, 108        // ascend
        };
        for(u64 i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
            s.on_bar(make_bar(static_cast<i64>(i) * 60000,
                              levels[i], levels[i], levels[i], levels[i], 1.0));
        }

        // The bottom fractal sits at index 5; detect_fractals requires
        // `window` bars on either side, so it fires once index 5+2 has
        // been ingested. Bars after that may or may not produce one
        // more (depends on absolute window), so we assert >=1 here.
        assert(s.fractals_seen_bottom >= 1);
        assert(s.entries_long >= 1);

        // We should now hold a long position.
        auto pos = s.acc.get_position("BTCUSDT"_v);
        assert(pos.ok());
        assert(pos->volume_long > 0.0);
    }

    Trace("Stop-loss triggers when price drops below the configured buffer") {
        CL::Chan_Live_Config cfg;
        cfg.symbol = "BTCUSDT"_v;
        cfg.fractal_window = 2;
        cfg.max_history_bars = 50;
        cfg.entry_cash_fraction = 0.5;
        cfg.stop_buffer_pct = 0.02;   // 2% below entry

        CL::Chan_Live_Strategy s{cfg};
        s.init_cash = 10000.0;
        s.acc = QS::Account<Mdefault>{"main"_v.string<Mdefault>(), 10000.0};
        // Use backtest mode so send_order also calls make_deal → the
        // position materialises immediately and the test can assert
        // on it without simulating a user-stream fill round-trip.
        s.running_mode = QS::Running_Mode::backtest;

        // Same V-shape entry as above.
        const f64 entry_levels[] = {
            108, 106, 104, 102, 100,
             98,
            102, 104, 106, 108
        };
        for(u64 i = 0; i < sizeof(entry_levels)/sizeof(entry_levels[0]); i++) {
            s.on_bar(make_bar(static_cast<i64>(i) * 60000,
                              entry_levels[i], entry_levels[i],
                              entry_levels[i], entry_levels[i], 1.0));
        }
        u64 entries_before = s.entries_long;
        u64 stops_before   = s.stop_hits;
        assert(entries_before >= 1);

        // Now drop price hard — the close must dip ≥2% below the entry
        // price to fire the stop. Entry was at price ~108 (the most
        // recent close), 2% below ≈ 105.84 — so push to 80.
        s.on_bar(make_bar(20 * 60000, 80, 80, 80, 80, 1.0));
        assert(s.stop_hits >= stops_before + 1);
        // Stop closes the long → no position.
        auto pos = s.acc.get_position("BTCUSDT"_v);
        if(pos.ok()) assert(pos->volume_long == 0.0);
    }

    return 0;
}
