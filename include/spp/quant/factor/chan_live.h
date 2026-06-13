#pragma once

// Incremental (streaming) Chan-theory feature builder.
//
// `Chan_Features::detect_fractals` walks every bar each call — fine for
// offline analysis but O(n·window) per bar when re-invoked on every new
// close, which is what a live 1-min strategy does. `Chan_Live_Features`
// keeps internal state so each `on_bar()` evaluates exactly one new
// fractal candidate (O(window)) and incrementally updates strokes and
// pivots, without recomputing the prefix that's already settled.
//
// Algorithmic correspondence to `Chan_Features` (one-shot):
//
//   - Fractal candidates are bars whose ±`window` neighbours are strictly
//     lower (for top) or higher (for bottom) than the candidate.
//   - When a same-type fractal appears consecutively in raw order, the
//     earlier one's price is updated to the more extreme value — exactly
//     matching the one-shot merge pass. Index intentionally stays put
//     (matches one-shot behaviour); same goes for time.
//   - When an opposite-type fractal arrives, a stroke between the two
//     is emitted if the price gap clears the `merge_threshold` band.
//   - A pivot is the (high, low) overlap of any three consecutive
//     strokes when high > low.
//
// State that must be invalidated when a same-type update edits the last
// fractal's price:
//
//   - The trailing stroke (if any) whose `end_idx` is that fractal's
//     absolute bar index — re-derive its `end_price` and `strength`.
//   - Any pivot whose stroke-triplet covers that stroke — pop and
//     re-evaluate. The cursor rewinds to the earliest triplet that
//     could be affected.
//
// Output Vecs (`fractals`, `strokes`, `pivots`) are append-only in the
// happy path and only trim from the tail on invalidation, so callers
// can hold references / indices across `on_bar` as long as they re-read
// after each call.

#include <spp/core/base.h>
#include <spp/quant/data/types.h>
#include <spp/quant/factor/chan_fractal.h>
#include <spp/quant/factor/math_util.h>

namespace spp::quant::factor {

template<typename A = Mdefault>
struct Chan_Live_Features {
    // ===== Configuration =====
    u32 fractal_window = 5;
    // Min relative price gap between adjacent opposite-type fractals
    // for a stroke to be emitted. Matches the one-shot detector's
    // 0.001 (0.1%) default; tighten for low-vol pairs.
    f64 merge_threshold = 0.001;

    // ===== Streaming state =====
    u64 bars_pushed = 0;

    Vec<Chan_Fractal, A> fractals;
    Vec<Chan_Stroke, A> strokes;
    Vec<Chan_Pivot, A> pivots;

    // For each emitted pivot, the index in `strokes` of its first stroke.
    // Used to pop and re-evaluate pivots when a trailing stroke mutates.
    Vec<u64, A> pivot_start_stroke_idx;

    // Stroke-builder cursor: next fractal index to evaluate as a stroke
    // anchor. The one-shot loop starts at i=1; we match that.
    u64 next_stroke_check = 1;
    // Pivot-builder cursor: next stroke triplet start index to evaluate.
    u64 next_pivot_check = 0;

    // Rolling bar window. We only need the last `2*window+1` bars in
    // memory — once a bar has the full ±window context, it's evaluated
    // and the leading entry can be dropped.
    struct Window_Bar {
        u64 abs_idx;
        Deterministic_Time time;
        Decimal<8> high_dec;
        Decimal<8> low_dec;
    };
    Vec<Window_Bar, A> bar_window;

    Chan_Live_Features() noexcept = default;
    explicit Chan_Live_Features(u32 win) noexcept : fractal_window(win) {
    }
    Chan_Live_Features(u32 win, f64 merge) noexcept
        : fractal_window(win), merge_threshold(merge) {
    }

    // Reset all derived state. The caller can re-feed a new sequence
    // from bar 0 afterwards.
    void reset() noexcept {
        bars_pushed = 0;
        fractals = Vec<Chan_Fractal, A>{};
        strokes = Vec<Chan_Stroke, A>{};
        pivots = Vec<Chan_Pivot, A>{};
        pivot_start_stroke_idx = Vec<u64, A>{};
        next_stroke_check = 1;
        next_pivot_check = 0;
        bar_window = Vec<Window_Bar, A>{};
    }

    // Push the next bar in chronological order. Fractals/strokes/pivots
    // are updated in-place; read them back after the call.
    void on_bar(const data::Bar& bar) noexcept {
        u64 abs_idx = bars_pushed;
        bars_pushed++;

        // 1. Append to the rolling window, trim if oversize.
        Window_Bar wb;
        wb.abs_idx = abs_idx;
        wb.time = bar.time;
        wb.high_dec = bar.high;
        wb.low_dec = bar.low;
        bar_window.push(spp::move(wb));

        u64 keep = static_cast<u64>(fractal_window) * 2 + 1;
        while(bar_window.length() > keep) {
            // Shift left by one. Window size is small (~11 for window=5),
            // so the O(W) shift dominates only when W is large; even at
            // W=20 it's still negligible per bar.
            for(u64 i = 1; i < bar_window.length(); i++) {
                bar_window[i - 1] = bar_window[i];
            }
            bar_window.pop();
        }

        // 2. Skip until we have full ±window context for the middle.
        if(bar_window.length() < keep) return;

        // 3. Evaluate the middle bar as a fractal candidate.
        u64 mid = fractal_window;
        f64 cur_h = data::price_to_f64(bar_window[mid].high_dec);
        f64 cur_l = data::price_to_f64(bar_window[mid].low_dec);
        bool is_top = true;
        bool is_bottom = true;
        for(u64 j = 0; j < bar_window.length(); j++) {
            if(j == mid) continue;
            if(data::price_to_f64(bar_window[j].high_dec) >= cur_h) is_top = false;
            if(data::price_to_f64(bar_window[j].low_dec) <= cur_l) is_bottom = false;
        }

        if(is_top) {
            Chan_Fractal f;
            f.index = bar_window[mid].abs_idx;
            f.time = bar_window[mid].time;
            f.type = Fractal_Type::top;
            f.price = bar_window[mid].high_dec;
            try_merge_fractal_(spp::move(f));
        }
        if(is_bottom) {
            Chan_Fractal f;
            f.index = bar_window[mid].abs_idx;
            f.time = bar_window[mid].time;
            f.type = Fractal_Type::bottom;
            f.price = bar_window[mid].low_dec;
            try_merge_fractal_(spp::move(f));
        }

        // 4. Advance derived state.
        drain_strokes_();
        drain_pivots_();
    }

private:
    // Mirror the one-shot merge logic: same type → keep more extreme
    // price (do NOT update index/time, matches the one-shot); different
    // type with sufficient gap → push as new fractal.
    void try_merge_fractal_(Chan_Fractal&& f) noexcept {
        if(fractals.length() == 0) {
            fractals.push(spp::move(f));
            return;
        }
        auto& prev = fractals[fractals.length() - 1];

        if(f.type == prev.type) {
            bool more_extreme =
                (f.type == Fractal_Type::top &&
                 data::price_to_f64(f.price) > data::price_to_f64(prev.price)) ||
                (f.type == Fractal_Type::bottom &&
                 data::price_to_f64(f.price) < data::price_to_f64(prev.price));
            if(more_extreme) {
                prev.price = f.price;
                // The trailing stroke (if any) anchored to this fractal
                // now has stale prices — rewrite it in place and rebuild
                // any pivots that swept across it.
                if(strokes.length() > 0) {
                    auto& last_s = strokes[strokes.length() - 1];
                    if(last_s.end_idx == prev.index) {
                        last_s.end_price = prev.price;
                        last_s.strength = Math::abs(
                            data::price_to_f64(last_s.end_price) -
                            data::price_to_f64(last_s.start_price));
                        invalidate_pivots_from_stroke_(strokes.length() - 1);
                    }
                }
            }
            return;
        }

        // Different type — push iff the gap clears the merge band.
        f64 gap = Math::abs(
            data::price_to_f64(f.price) - data::price_to_f64(prev.price));
        f64 avg_px = (data::price_to_f64(f.price) +
                      data::price_to_f64(prev.price)) * 0.5;
        if(avg_px > 1e-12 && gap / avg_px > merge_threshold) {
            fractals.push(spp::move(f));
        }
    }

    // Emit strokes for any newly-paired fractals. The one-shot loop
    // walks `for(i=1; i+1<n; i++)` and advances i by 2 on a successful
    // pairing, by 1 otherwise — we replicate that semantics via a
    // single cursor that drains as far as the data allows.
    void drain_strokes_() noexcept {
        while(next_stroke_check + 1 < fractals.length()) {
            u64 i = next_stroke_check;
            if(fractals[i].type != fractals[i + 1].type) {
                Chan_Stroke s;
                s.start_idx = fractals[i].index;
                s.end_idx = fractals[i + 1].index;
                s.direction = fractals[i].type == Fractal_Type::bottom
                    ? Fractal_Type::top : Fractal_Type::bottom;
                s.start_price = fractals[i].price;
                s.end_price = fractals[i + 1].price;
                s.strength = Math::abs(
                    data::price_to_f64(s.end_price) -
                    data::price_to_f64(s.start_price));
                strokes.push(spp::move(s));
                next_stroke_check = i + 2;
            } else {
                next_stroke_check = i + 1;
            }
        }
    }

    // Evaluate all stroke triplets the cursor hasn't visited yet; emit a
    // pivot whenever the three strokes overlap (their min-high above
    // their max-low). Cursor advances by 1 per triplet regardless of
    // whether a pivot was emitted — matches the one-shot loop.
    void drain_pivots_() noexcept {
        while(next_pivot_check + 3 <= strokes.length()) {
            u64 i = next_pivot_check;
            f64 s1_h = Math::max(data::price_to_f64(strokes[i].start_price),
                                  data::price_to_f64(strokes[i].end_price));
            f64 s1_l = Math::min(data::price_to_f64(strokes[i].start_price),
                                  data::price_to_f64(strokes[i].end_price));
            f64 s2_h = Math::max(data::price_to_f64(strokes[i + 1].start_price),
                                  data::price_to_f64(strokes[i + 1].end_price));
            f64 s2_l = Math::min(data::price_to_f64(strokes[i + 1].start_price),
                                  data::price_to_f64(strokes[i + 1].end_price));
            f64 s3_h = Math::max(data::price_to_f64(strokes[i + 2].start_price),
                                  data::price_to_f64(strokes[i + 2].end_price));
            f64 s3_l = Math::min(data::price_to_f64(strokes[i + 2].start_price),
                                  data::price_to_f64(strokes[i + 2].end_price));
            f64 ph = Math::min({s1_h, s2_h, s3_h});
            f64 pl = Math::max({s1_l, s2_l, s3_l});
            if(ph > pl) {
                Chan_Pivot p;
                p.high = data::f64_to_price(ph);
                p.low = data::f64_to_price(pl);
                p.start_idx = strokes[i].start_idx;
                p.end_idx = strokes[i + 2].end_idx;
                pivots.push(spp::move(p));
                pivot_start_stroke_idx.push(i);
            }
            next_pivot_check = i + 1;
        }
    }

    // Pop trailing pivots whose stroke triplet contains `stroke_idx`,
    // then rewind the pivot cursor far enough to re-evaluate every
    // triplet that could be affected. `drain_pivots_` rebuilds.
    void invalidate_pivots_from_stroke_(u64 stroke_idx) noexcept {
        while(pivot_start_stroke_idx.length() > 0) {
            u64 k = pivot_start_stroke_idx[pivot_start_stroke_idx.length() - 1];
            if(k + 2 >= stroke_idx) {
                pivot_start_stroke_idx.pop();
                pivots.pop();
            } else {
                break;
            }
        }
        u64 rewind_to = stroke_idx >= 2 ? stroke_idx - 2 : 0;
        if(next_pivot_check > rewind_to) next_pivot_check = rewind_to;
        drain_pivots_();
    }
};

} // namespace spp::quant::factor
