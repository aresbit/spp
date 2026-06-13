#include "test.h"

#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/factor/chan_fractal.h>
#include <spp/quant/factor/chan_live.h>

namespace QF = spp::quant::factor;
namespace QD = spp::quant::data;

static QD::Symbol_Bar make_sb(spp::i64 t_ms, f64 o, f64 h, f64 l, f64 c) noexcept {
    QD::Symbol_Bar sb;
    sb.symbol = "BTCUSDT"_v.string<Mdefault>();
    sb.bar.time = spp::Deterministic_Time::from_unix_ms(t_ms);
    sb.bar.open  = QD::f64_to_price(o);
    sb.bar.high  = QD::f64_to_price(h);
    sb.bar.low   = QD::f64_to_price(l);
    sb.bar.close = QD::f64_to_price(c);
    sb.bar.volume = 1.0;
    return sb;
}

// Drive incremental detector with each bar from `data`.
template<typename A>
static void feed_incremental(QF::Chan_Live_Features<A>& live,
                              const QD::Ohlcv_Data<A>& data) noexcept {
    for(spp::u64 i = 0; i < data.bars.length(); i++) {
        live.on_bar(data.bars[i].bar);
    }
}

// Compare two fractal vectors for structural equivalence: same length,
// same indices, types, and prices.
template<typename A>
static void
assert_fractals_match(const spp::Vec<QF::Chan_Fractal, A>& a,
                       const spp::Vec<QF::Chan_Fractal, A>& b) noexcept {
    assert(a.length() == b.length());
    for(spp::u64 i = 0; i < a.length(); i++) {
        assert(a[i].index == b[i].index);
        assert(a[i].type == b[i].type);
        assert(a[i].price.raw() == b[i].price.raw());
    }
}

template<typename A>
static void
assert_strokes_match(const spp::Vec<QF::Chan_Stroke, A>& a,
                      const spp::Vec<QF::Chan_Stroke, A>& b) noexcept {
    assert(a.length() == b.length());
    for(spp::u64 i = 0; i < a.length(); i++) {
        assert(a[i].start_idx == b[i].start_idx);
        assert(a[i].end_idx == b[i].end_idx);
        assert(a[i].direction == b[i].direction);
        assert(a[i].start_price.raw() == b[i].start_price.raw());
        assert(a[i].end_price.raw() == b[i].end_price.raw());
    }
}

template<typename A>
static void
assert_pivots_match(const spp::Vec<QF::Chan_Pivot, A>& a,
                     const spp::Vec<QF::Chan_Pivot, A>& b) noexcept {
    assert(a.length() == b.length());
    for(spp::u64 i = 0; i < a.length(); i++) {
        assert(a[i].start_idx == b[i].start_idx);
        assert(a[i].end_idx == b[i].end_idx);
        assert(a[i].high.raw() == b[i].high.raw());
        assert(a[i].low.raw() == b[i].low.raw());
    }
}

i32 main() {
    Test test{"empty"_v};

    Trace("Parity: V-shape produces one bottom fractal in both detectors") {
        // Symmetric V: descend, hit floor, ascend. window=2 means we need
        // ≥5 bars to evaluate a fractal candidate.
        QD::Ohlcv_Data<Mdefault> data;
        const f64 prices[] = {110, 108, 106, 104, 102, 100, 98,
                              100, 102, 104, 106, 108, 110};
        for(spp::u64 i = 0; i < sizeof(prices) / sizeof(prices[0]); i++) {
            data.bars.push(make_sb(static_cast<spp::i64>(i) * 60000,
                                    prices[i], prices[i], prices[i], prices[i]));
        }

        auto oneshot_f = QF::Chan_Features<Mdefault>::detect_fractals(data, 2);
        auto oneshot_s = QF::Chan_Features<Mdefault>::build_strokes(oneshot_f);
        auto oneshot_p = QF::Chan_Features<Mdefault>::build_pivots(oneshot_s);

        QF::Chan_Live_Features<Mdefault> live{2};
        feed_incremental(live, data);

        assert_fractals_match(live.fractals, oneshot_f);
        assert_strokes_match(live.strokes, oneshot_s);
        assert_pivots_match(live.pivots, oneshot_p);
    }

    Trace("Parity: zigzag with multiple alternating fractals") {
        // Build a sequence that produces several fractals of alternating
        // types. The window=2 is tight enough to make ridges and troughs
        // pop out clearly.
        QD::Ohlcv_Data<Mdefault> data;
        // Peak at idx 2; trough at idx 7; peak at idx 12; trough at idx 17.
        const f64 prices[] = {
            100, 105, 110, 105, 100, 95, 90, 85, 90, 95,
            100, 105, 115, 110, 105, 100, 95, 80, 85, 90, 95
        };
        for(spp::u64 i = 0; i < sizeof(prices) / sizeof(prices[0]); i++) {
            data.bars.push(make_sb(static_cast<spp::i64>(i) * 60000,
                                    prices[i], prices[i], prices[i], prices[i]));
        }

        auto oneshot_f = QF::Chan_Features<Mdefault>::detect_fractals(data, 2);
        auto oneshot_s = QF::Chan_Features<Mdefault>::build_strokes(oneshot_f);
        auto oneshot_p = QF::Chan_Features<Mdefault>::build_pivots(oneshot_s);

        QF::Chan_Live_Features<Mdefault> live{2};
        feed_incremental(live, data);

        assert_fractals_match(live.fractals, oneshot_f);
        assert_strokes_match(live.strokes, oneshot_s);
        assert_pivots_match(live.pivots, oneshot_p);
    }

    Trace("Parity: same-type consecutive fractals merge to the more extreme") {
        // Two bottom fractals appear consecutively without an intervening
        // top — the more-extreme one's price should be kept on the FIRST
        // confirmed fractal (one-shot behaviour). This exercises the
        // incremental detector's prev-fractal update path.
        QD::Ohlcv_Data<Mdefault> data;
        // First bottom @ idx 2 (price 90). Brief rise then a DEEPER bottom
        // @ idx 7 (price 85). The original detector merges them so the
        // single confirmed bottom carries price 85, but the dip between
        // them isn't deep enough to trigger an intervening top.
        const f64 prices[] = {
            96, 93, 90, 93, 96, 93, 90, 85, 90, 93,
            96, 99, 102, 105, 108
        };
        for(spp::u64 i = 0; i < sizeof(prices) / sizeof(prices[0]); i++) {
            data.bars.push(make_sb(static_cast<spp::i64>(i) * 60000,
                                    prices[i], prices[i], prices[i], prices[i]));
        }

        auto oneshot_f = QF::Chan_Features<Mdefault>::detect_fractals(data, 2);
        auto oneshot_s = QF::Chan_Features<Mdefault>::build_strokes(oneshot_f);

        QF::Chan_Live_Features<Mdefault> live{2};
        feed_incremental(live, data);

        assert_fractals_match(live.fractals, oneshot_f);
        assert_strokes_match(live.strokes, oneshot_s);
    }

    Trace("Reset returns to clean state; second feed produces same result") {
        QD::Ohlcv_Data<Mdefault> data;
        const f64 prices[] = {100, 105, 110, 105, 100, 95, 90, 95, 100};
        for(spp::u64 i = 0; i < sizeof(prices) / sizeof(prices[0]); i++) {
            data.bars.push(make_sb(static_cast<spp::i64>(i) * 60000,
                                    prices[i], prices[i], prices[i], prices[i]));
        }

        QF::Chan_Live_Features<Mdefault> live{2};
        feed_incremental(live, data);
        auto fractals_first = live.fractals.length();
        auto strokes_first = live.strokes.length();

        live.reset();
        assert(live.fractals.length() == 0);
        assert(live.strokes.length() == 0);
        assert(live.bars_pushed == 0);

        feed_incremental(live, data);
        assert(live.fractals.length() == fractals_first);
        assert(live.strokes.length() == strokes_first);
    }

    return 0;
}
