#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/factor/rolling.h>
#include <spp/quant/factor/math_util.h>

namespace spp::quant::factor {

using data::price_to_f64;

struct Chan_Fractal;
struct Chan_Stroke;
struct Chan_Segment;
struct Chan_Pivot;

enum class Fractal_Type : u8 { none, top, bottom };

struct Chan_Fractal {
    u64 index;
    Deterministic_Time time;
    Fractal_Type type;
    Decimal<8> price;
};

struct Chan_Stroke {
    u64 start_idx;
    u64 end_idx;
    Fractal_Type direction;
    Decimal<8> start_price;
    Decimal<8> end_price;
    f64 strength;
};

struct Chan_Segment {
    u64 start_idx;
    u64 end_idx;
    Vec<Chan_Stroke, Mdefault> strokes;
};

struct Chan_Pivot {
    Decimal<8> high;
    Decimal<8> low;
    u64 start_idx;
    u64 end_idx;
};

template <typename A = Mdefault>
struct Chan_Features {

    static auto detect_fractals(const data::Ohlcv_Data<A>& data, u32 window = 5)
        -> Vec<Chan_Fractal, A>;

    static auto build_strokes(const Vec<Chan_Fractal, A>& fractals)
        -> Vec<Chan_Stroke, A>;

    static auto build_segments(const Vec<Chan_Stroke, A>& strokes)
        -> Vec<Chan_Segment, A>;

    static auto build_pivots(const Vec<Chan_Stroke, A>& strokes)
        -> Vec<Chan_Pivot, A>;

    static auto detect_trade_points(const data::Ohlcv_Data<A>& data,
                                     const Vec<Chan_Pivot, A>& pivots,
                                     const Vec<Chan_Fractal, A>& fractals) -> Vec<f64, A>;

    static auto divergence_score(const data::Ohlcv_Data<A>& data,
                                  const Vec<Chan_Stroke, A>& strokes) -> Vec<f64, A>;

    static auto extract_all(const data::Ohlcv_Data<A>& data)
        -> Map<String<A>, Vec<f64, A>, A>;
};

} // namespace spp::quant::factor

SPP_NAMED_ENUM(spp::quant::factor::Fractal_Type, "QF_Fractal_Type", none,
    SPP_CASE(top), SPP_CASE(bottom));

SPP_NAMED_RECORD(spp::quant::factor::Chan_Fractal, "QF_Chan_Fractal",
    SPP_FIELD(index), SPP_FIELD(time), SPP_FIELD(type), SPP_FIELD(price));

SPP_NAMED_RECORD(spp::quant::factor::Chan_Stroke, "QF_Chan_Stroke",
    SPP_FIELD(start_idx), SPP_FIELD(end_idx), SPP_FIELD(direction),
    SPP_FIELD(start_price), SPP_FIELD(end_price), SPP_FIELD(strength));

SPP_NAMED_RECORD(spp::quant::factor::Chan_Segment, "QF_Chan_Segment",
    SPP_FIELD(start_idx), SPP_FIELD(end_idx), SPP_FIELD(strokes));

SPP_NAMED_RECORD(spp::quant::factor::Chan_Pivot, "QF_Chan_Pivot",
    SPP_FIELD(high), SPP_FIELD(low), SPP_FIELD(start_idx), SPP_FIELD(end_idx));

namespace spp::quant::factor {

template <typename A>
auto Chan_Features<A>::detect_fractals(const data::Ohlcv_Data<A>& data, u32 window)
    -> Vec<Chan_Fractal, A> {
    u64 n = data.bars.length();
    if (n < window * 2 + 1) {
        auto empty = Vec<Chan_Fractal, A>{};
        empty.reserve(0);
        return empty;
    }
    u32 half_w = window;
    Vec<Chan_Fractal, A> raw;
    raw.reserve(n);
    for (u64 i = half_w; i + half_w < n; i++) {
        bool is_top = true;
        bool is_bottom = true;
        f64 cur_h = price_to_f64(data.bars[i].bar.high);
        f64 cur_l = price_to_f64(data.bars[i].bar.low);
        for (u64 j = i - half_w; j <= i + half_w; j++) {
            if (j == i) continue;
            if (price_to_f64(data.bars[j].bar.high) >= cur_h) is_top = false;
            if (price_to_f64(data.bars[j].bar.low) <= cur_l) is_bottom = false;
        }
        if (is_top) {
            Chan_Fractal f;
            f.index = i;
            f.time = data.bars[i].bar.time;
            f.type = Fractal_Type::top;
            f.price = data.bars[i].bar.high;
            raw.push(spp::move(f));
        }
        if (is_bottom) {
            Chan_Fractal f;
            f.index = i;
            f.time = data.bars[i].bar.time;
            f.type = Fractal_Type::bottom;
            f.price = data.bars[i].bar.low;
            raw.push(spp::move(f));
        }
    }
    u64 rf_len = raw.length();
    Vec<Chan_Fractal, A> confirmed;
    confirmed.reserve(rf_len > 0 ? rf_len : 1);
    for (u64 i = 0; i < rf_len; i++) {
        if (i == 0) {
            Chan_Fractal copy{raw[i]};
            confirmed.push(spp::move(copy));
            continue;
        }
        auto& prev = confirmed.back();
        auto& cur = raw[i];
        if (cur.type == prev.type) {
            if (cur.type == Fractal_Type::top
                && price_to_f64(cur.price) > price_to_f64(prev.price))
                prev.price = cur.price;
            else if (cur.type == Fractal_Type::bottom
                     && price_to_f64(cur.price) < price_to_f64(prev.price))
                prev.price = cur.price;
        } else {
            f64 gap = Math::abs(price_to_f64(cur.price) - price_to_f64(prev.price));
            f64 avg_px = (price_to_f64(cur.price) + price_to_f64(prev.price)) * 0.5;
            if (avg_px > 1e-12 && gap / avg_px > 0.001) {
                Chan_Fractal copy{cur};
                confirmed.push(spp::move(copy));
            }
        }
    }
    return confirmed;
}

template <typename A>
auto Chan_Features<A>::build_strokes(const Vec<Chan_Fractal, A>& fractals)
    -> Vec<Chan_Stroke, A> {
    u64 n = fractals.length();
    Vec<Chan_Stroke, A> strokes;
    strokes.reserve(n > 2 ? n / 2 : 0);
    for (u64 i = 1; i + 1 < n; i++) {
        if (fractals[i].type != fractals[i + 1].type) {
            Chan_Stroke stroke;
            stroke.start_idx = fractals[i].index;
            stroke.end_idx = fractals[i + 1].index;
            stroke.direction = fractals[i].type == Fractal_Type::bottom
                                   ? Fractal_Type::top : Fractal_Type::bottom;
            stroke.start_price = fractals[i].price;
            stroke.end_price = fractals[i + 1].price;
            stroke.strength = Math::abs(
                price_to_f64(fractals[i + 1].price) - price_to_f64(fractals[i].price));
            strokes.push(spp::move(stroke));
            i++;
        }
    }
    return strokes;
}

template <typename A>
auto Chan_Features<A>::build_segments(const Vec<Chan_Stroke, A>& strokes)
    -> Vec<Chan_Segment, A> {
    u64 n = strokes.length();
    Vec<Chan_Segment, A> segments;
    segments.reserve(n > 3 ? n / 3 : 0);
    for (u64 i = 0; i + 3 <= n; i += 3) {
        Chan_Segment seg;
        seg.start_idx = strokes[i].start_idx;
        seg.end_idx = strokes[i + 2].end_idx;
        seg.strokes.reserve(3);
        for (u64 k = 0; k < 3; k++) {
            Chan_Stroke s{strokes[i + k]};
            seg.strokes.push(spp::move(s));
        }
        segments.push(spp::move(seg));
    }
    return segments;
}

template <typename A>
auto Chan_Features<A>::build_pivots(const Vec<Chan_Stroke, A>& strokes)
    -> Vec<Chan_Pivot, A> {
    u64 n = strokes.length();
    Vec<Chan_Pivot, A> pivots;
    pivots.reserve(n > 3 ? n / 3 : 0);
    for (u64 i = 0; i + 3 <= n; i++) {
        f64 s1_h = Math::max(price_to_f64(strokes[i].start_price),
                              price_to_f64(strokes[i].end_price));
        f64 s1_l = Math::min(price_to_f64(strokes[i].start_price),
                              price_to_f64(strokes[i].end_price));
        f64 s2_h = Math::max(price_to_f64(strokes[i + 1].start_price),
                              price_to_f64(strokes[i + 1].end_price));
        f64 s2_l = Math::min(price_to_f64(strokes[i + 1].start_price),
                              price_to_f64(strokes[i + 1].end_price));
        f64 s3_h = Math::max(price_to_f64(strokes[i + 2].start_price),
                              price_to_f64(strokes[i + 2].end_price));
        f64 s3_l = Math::min(price_to_f64(strokes[i + 2].start_price),
                              price_to_f64(strokes[i + 2].end_price));
        f64 ph = Math::min({s1_h, s2_h, s3_h});
        f64 pl = Math::max({s1_l, s2_l, s3_l});
        if (ph > pl) {
            Chan_Pivot p;
            p.high = data::f64_to_price(ph);
            p.low = data::f64_to_price(pl);
            p.start_idx = strokes[i].start_idx;
            p.end_idx = strokes[i + 2].end_idx;
            pivots.push(spp::move(p));
        }
    }
    return pivots;
}

template <typename A>
auto Chan_Features<A>::detect_trade_points(const data::Ohlcv_Data<A>& data,
                                             const Vec<Chan_Pivot, A>& pivots,
                                             const Vec<Chan_Fractal, A>&) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto scores = Vec<f64, A>::make(n);
    for (u64 pi = 0; pi < pivots.length(); pi++) {
        const auto& p = pivots[pi];
        f64 p_high = price_to_f64(p.high);
        f64 p_low = price_to_f64(p.low);
        if (pi > 0 && pi + 1 < pivots.length()) {
            if (p_low > price_to_f64(pivots[pi - 1].high)
                && price_to_f64(pivots[pi + 1].low) > p_high) {
                for (u64 idx = p.start_idx; idx <= p.end_idx && idx < n; idx++)
                    scores[idx] += 1.0;
            }
        }
        if (pi > 0) {
            if (price_to_f64(pivots[pi - 1].low) > p_high) {
                for (u64 idx = p.start_idx; idx <= p.end_idx && idx < n; idx++)
                    scores[idx] -= 1.0;
            }
        }
    }
    return scores;
}

template <typename A>
auto Chan_Features<A>::divergence_score(const data::Ohlcv_Data<A>& data,
                                          const Vec<Chan_Stroke, A>& strokes) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto scores = Vec<f64, A>::make(n);
    for (u64 si = 0; si + 1 < strokes.length(); si++) {
        f64 s1_dir = price_to_f64(strokes[si].end_price)
                     - price_to_f64(strokes[si].start_price);
        f64 s2_dir = price_to_f64(strokes[si + 1].end_price)
                     - price_to_f64(strokes[si + 1].start_price);
        if (s1_dir * s2_dir < 0) {
            f64 s1_mag = Math::abs(s1_dir);
            f64 s2_mag = Math::abs(s2_dir);
            f64 div = s1_mag + s2_mag;
            if (div > 0) {
                f64 sc = (s1_dir > 0) ? (s2_mag / div) : (-s2_mag / div);
                for (u64 idx = strokes[si + 1].start_idx;
                     idx <= strokes[si + 1].end_idx && idx < n; idx++)
                    scores[idx] += sc;
            }
        }
    }
    return scores;
}

template <typename A>
auto Chan_Features<A>::extract_all(const data::Ohlcv_Data<A>& data)
    -> Map<String<A>, Vec<f64, A>, A> {
    Map<String<A>, Vec<f64, A>, A> features;
    auto fractals = detect_fractals(data);
    u64 n = data.bars.length();

    auto fc = Vec<f64, A>::make(1);
    fc[0] = static_cast<f64>(fractals.length());
    features.insert("fractal_count"_v.template string<A>(), spp::move(fc));

    auto fscore = Vec<f64, A>::make(n);
    for (u64 i = 0; i < fractals.length(); i++) {
        u64 idx = fractals[i].index;
        if (idx < n)
            fscore[idx] = (fractals[i].type == Fractal_Type::top) ? 1.0 : -1.0;
    }
    features.insert("fractal_score"_v.template string<A>(), spp::move(fscore));

    auto strokes = build_strokes(fractals);
    auto sc = Vec<f64, A>::make(1);
    sc[0] = static_cast<f64>(strokes.length());
    features.insert("stroke_count"_v.template string<A>(), spp::move(sc));

    if (strokes.length() >= 3) {
        auto segments = build_segments(strokes);
        auto sg = Vec<f64, A>::make(1);
        sg[0] = static_cast<f64>(segments.length());
        features.insert("segment_count"_v.template string<A>(), spp::move(sg));

        auto pivots = build_pivots(strokes);
        auto pv = Vec<f64, A>::make(1);
        pv[0] = static_cast<f64>(pivots.length());
        features.insert("pivot_count"_v.template string<A>(), spp::move(pv));

        auto tp = detect_trade_points(data, pivots, fractals);
        features.insert("trade_point_score"_v.template string<A>(), spp::move(tp));
    }

    auto div = divergence_score(data, strokes);
    features.insert("divergence_score"_v.template string<A>(), spp::move(div));

    return features;
}

} // namespace spp::quant::factor
