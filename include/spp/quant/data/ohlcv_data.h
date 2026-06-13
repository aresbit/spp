#pragma once
#include <spp/core/base.h>
#include <spp/quant/data/types.h>

namespace spp::quant::data {

struct Bar {
    Deterministic_Time time;
    Decimal<8> open;
    Decimal<8> high;
    Decimal<8> low;
    Decimal<8> close;
    f64 volume;
};

struct Symbol_Bar {
    String<Mdefault> symbol;
    Bar bar;
};

template <typename A = Mdefault>
struct Ohlcv_Data {
    Vec<Symbol_Bar, A> bars;
    String<A> frequency;
    String<A> market_type;

    Ohlcv_Data() noexcept = default;

    Ohlcv_Data(Ohlcv_Data&&) noexcept = default;
    Ohlcv_Data& operator=(Ohlcv_Data&&) noexcept = default;

    [[nodiscard]] u64 bar_count() const noexcept {
        return bars.length();
    }

    [[nodiscard]] u64 symbol_count() const noexcept {
        Map<String<Mdefault>, u8, A> seen;
        for(u64 i = 0; i < bars.length(); i++) {
            String_View sv = bars[i].symbol.view();
            if(!seen.contains(sv)) {
                seen.insert(bars[i].symbol.clone(), 1);
            }
        }
        return seen.length();
    }

    [[nodiscard]] Vec<String<A>, A> symbols() const noexcept {
        Map<String<Mdefault>, u8, A> seen;
        Vec<String<A>, A> out;
        for(u64 i = 0; i < bars.length(); i++) {
            String_View sv = bars[i].symbol.view();
            if(!seen.contains(sv)) {
                seen.insert(bars[i].symbol.clone(), 1);
                out.push(sv.template string<A>());
            }
        }
        return out;
    }

    [[nodiscard]] Vec<Deterministic_Time, A> unique_times() const noexcept {
        Map<Deterministic_Time, u8, A> seen;
        for(u64 i = 0; i < bars.length(); i++) {
            auto t = bars[i].bar.time;
            if(!seen.contains(t)) {
                seen.insert(Deterministic_Time::from_unix_ns(t.unix_ns()), static_cast<u8>(1));
            }
        }
        Vec<Deterministic_Time, A> out;
        for(const auto& entry : seen) {
            out.push(Deterministic_Time::from_unix_ns(entry.first.unix_ns()));
        }
        return out;
    }

    [[nodiscard]] Map<String<A>, Vec<f64, A>, A> close_prices() const noexcept {
        Map<String<A>, Vec<f64, A>, A> result;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = result.try_get(sv);
            if(opt.ok()) {
                (**opt).push(price_to_f64(sb.bar.close));
            } else {
                Vec<f64, A> v;
                v.push(price_to_f64(sb.bar.close));
                result.insert(sv.template string<A>(), spp::move(v));
            }
        }
        return result;
    }

    [[nodiscard]] Map<String<A>, Vec<f64, A>, A> open_prices() const noexcept {
        Map<String<A>, Vec<f64, A>, A> result;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = result.try_get(sv);
            if(opt.ok()) {
                (**opt).push(price_to_f64(sb.bar.open));
            } else {
                Vec<f64, A> v;
                v.push(price_to_f64(sb.bar.open));
                result.insert(sv.template string<A>(), spp::move(v));
            }
        }
        return result;
    }

    [[nodiscard]] Map<String<A>, Vec<f64, A>, A> high_prices() const noexcept {
        Map<String<A>, Vec<f64, A>, A> result;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = result.try_get(sv);
            if(opt.ok()) {
                (**opt).push(price_to_f64(sb.bar.high));
            } else {
                Vec<f64, A> v;
                v.push(price_to_f64(sb.bar.high));
                result.insert(sv.template string<A>(), spp::move(v));
            }
        }
        return result;
    }

    [[nodiscard]] Map<String<A>, Vec<f64, A>, A> low_prices() const noexcept {
        Map<String<A>, Vec<f64, A>, A> result;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = result.try_get(sv);
            if(opt.ok()) {
                (**opt).push(price_to_f64(sb.bar.low));
            } else {
                Vec<f64, A> v;
                v.push(price_to_f64(sb.bar.low));
                result.insert(sv.template string<A>(), spp::move(v));
            }
        }
        return result;
    }

    [[nodiscard]] Map<String<A>, Vec<f64, A>, A> volumes() const noexcept {
        Map<String<A>, Vec<f64, A>, A> result;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = result.try_get(sv);
            if(opt.ok()) {
                (**opt).push(sb.bar.volume);
            } else {
                Vec<f64, A> v;
                v.push(sb.bar.volume);
                result.insert(sv.template string<A>(), spp::move(v));
            }
        }
        return result;
    }

    [[nodiscard]] Ohlcv_Data<A> select_code(String_View code) const noexcept {
        Ohlcv_Data<A> result;
        result.frequency = frequency.clone();
        result.market_type = market_type.clone();
        for(u64 i = 0; i < bars.length(); i++) {
            if(bars[i].symbol == code) {
                Symbol_Bar sb;
                sb.symbol = bars[i].symbol.clone();
                sb.bar = bars[i].bar;
                result.bars.push(spp::move(sb));
            }
        }
        return result;
    }

    [[nodiscard]] Ohlcv_Data<A> select_time(Deterministic_Time start,
                                            Deterministic_Time end) const noexcept {
        Ohlcv_Data<A> result;
        result.frequency = frequency.clone();
        result.market_type = market_type.clone();
        for(u64 i = 0; i < bars.length(); i++) {
            auto t = bars[i].bar.time;
            if(!(t < start) && t < end) {
                Symbol_Bar sb;
                sb.symbol = bars[i].symbol.clone();
                sb.bar = bars[i].bar;
                result.bars.push(spp::move(sb));
            }
        }
        return result;
    }

    // Group bars by timestamp, sorted chronologically. The backtest engine
    // consumes this to drive its event loop one timestamp group at a time
    // (rebalance decisions need cross-sectional context — i.e. all symbols
    // priced at the same `t`).
    [[nodiscard]] Vec<Pair<Deterministic_Time, Vec<Symbol_Bar, A>>, A>
    group_by_time() const noexcept {
        Vec<Pair<Deterministic_Time, Vec<Symbol_Bar, A>>, A> out;
        // Two-pass to avoid Map<Time, ...> rehash + later sort. Bars in
        // realistic inputs are already chronologically ordered, so the
        // hot path is "is the next bar in the current bucket?".
        Deterministic_Time last_t = Deterministic_Time::from_unix_ns(0);
        bool have_last = false;
        for(u64 i = 0; i < bars.length(); i++) {
            auto t = bars[i].bar.time;
            Symbol_Bar sb;
            sb.symbol = bars[i].symbol.clone();
            sb.bar = bars[i].bar;
            if(have_last && t.unix_ns() == last_t.unix_ns()) {
                out[out.length() - 1].second.push(spp::move(sb));
                continue;
            }
            // New bucket — slow path: search existing groups (input may be
            // unsorted in pathological cases). Linear scan is fine; the
            // outer loop is O(bars) and we expect ≤ a few thousand groups.
            bool merged = false;
            for(u64 j = 0; j < out.length(); j++) {
                if(out[j].first.unix_ns() == t.unix_ns()) {
                    out[j].second.push(spp::move(sb));
                    merged = true;
                    break;
                }
            }
            if(!merged) {
                Vec<Symbol_Bar, A> v;
                v.push(spp::move(sb));
                out.push(Pair<Deterministic_Time, Vec<Symbol_Bar, A>>{t, spp::move(v)});
            }
            last_t = t;
            have_last = true;
        }
        // Sort chronologically. Bubble sort: bar counts in realistic 1-min
        // backtests are small (≤ a few thousand groups) and we usually
        // start sorted. For >10k groups, swap for a real sort.
        for(u64 i = 0; i < out.length(); i++) {
            for(u64 j = i + 1; j < out.length(); j++) {
                if(out[j].first.unix_ns() < out[i].first.unix_ns()) {
                    auto tmp = spp::move(out[i]);
                    out[i] = spp::move(out[j]);
                    out[j] = spp::move(tmp);
                }
            }
        }
        return out;
    }

    // Average bar volume for `symbol`, or 0 if the symbol has no history.
    // Backtest's impact / slippage models need a reference volume; this
    // gives them a data-derived estimate instead of a hard-coded constant.
    [[nodiscard]] f64 mean_volume(String_View symbol) const noexcept {
        f64 sum = 0.0;
        u64 count = 0;
        for(u64 i = 0; i < bars.length(); i++) {
            if(bars[i].symbol == symbol) {
                sum += bars[i].bar.volume;
                count++;
            }
        }
        return count > 0 ? sum / static_cast<f64>(count) : 0.0;
    }

    [[nodiscard]] Map<Deterministic_Time, Map<String<A>, f64, A>, A> close_panel() const noexcept {
        Map<Deterministic_Time, Map<String<A>, f64, A>, A> panel;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            auto t = sb.bar.time;
            f64 px = price_to_f64(sb.bar.close);

            auto existing = panel.try_get(t);
            if(existing.ok()) {
                (**existing).insert(sb.symbol.view().template string<A>(), f64{px});
            } else {
                Map<String<A>, f64, A> inner;
                inner.insert(sb.symbol.view().template string<A>(), f64{px});
                panel.insert(Deterministic_Time::from_unix_ns(t.unix_ns()), spp::move(inner));
            }
        }
        return panel;
    }

    [[nodiscard]] Ohlcv_Data<A> pct_change(u64 periods = 1) const noexcept {
        if(bar_count() == 0) return {};

        Ohlcv_Data<A> result;
        result.frequency = frequency.clone();
        result.market_type = market_type.clone();

        Map<String<Mdefault>, Vec<Pair<Deterministic_Time, f64>, A>, A> series;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = series.try_get(sv);
            Pair<Deterministic_Time, f64> p{
                Deterministic_Time::from_unix_ns(sb.bar.time.unix_ns()),
                price_to_f64(sb.bar.close)};
            if(opt.ok()) {
                (**opt).push(spp::move(p));
            } else {
                Vec<Pair<Deterministic_Time, f64>, A> v;
                v.push(spp::move(p));
                series.insert(sb.symbol.clone(), spp::move(v));
            }
        }

        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            auto opt = series.try_get(sv);
            if(!opt.ok()) continue;

            auto& ts_data = **opt;
            u64 pos = 0;
            for(u64 k = 0; k < ts_data.length(); k++) {
                if(ts_data[k].first.unix_ns() == sb.bar.time.unix_ns()) {
                    pos = k;
                    break;
                }
            }

            if(pos < periods) {
                Symbol_Bar out_sb;
                out_sb.symbol = sb.symbol.clone();
                out_sb.bar.time = sb.bar.time;
                out_sb.bar.open = Decimal<8>::from_int(0);
                out_sb.bar.high = Decimal<8>::from_int(0);
                out_sb.bar.low = Decimal<8>::from_int(0);
                out_sb.bar.close = Decimal<8>::from_int(0);
                out_sb.bar.volume = 0.0;
                result.bars.push(spp::move(out_sb));
                continue;
            }

            f64 prev_close = ts_data[pos - periods].second;
            f64 cur_close = price_to_f64(sb.bar.close);
            f64 chg = prev_close != 0.0 ? (cur_close - prev_close) / prev_close * 100.0 : 0.0;
            Decimal<8> dec_chg = f64_to_price(chg);

            Symbol_Bar out_sb;
            out_sb.symbol = sb.symbol.clone();
            out_sb.bar.time = sb.bar.time;
            out_sb.bar.open = dec_chg;
            out_sb.bar.high = dec_chg;
            out_sb.bar.low = dec_chg;
            out_sb.bar.close = dec_chg;
            out_sb.bar.volume = sb.bar.volume;
            result.bars.push(spp::move(out_sb));
        }
        return result;
    }

    [[nodiscard]] Vec<f64, A> log_returns() const noexcept {
        Vec<f64, A> out;
        if(bar_count() < 2) return out;

        Map<String<Mdefault>, f64, A> prev_close;
        for(u64 i = 0; i < bars.length(); i++) {
            auto& sb = bars[i];
            String_View sv = sb.symbol.view();
            f64 px = price_to_f64(sb.bar.close);
            auto opt = prev_close.try_get(sv);
            if(opt.ok() && **opt != 0.0) {
                if(px > 0.0) {
                    f64 lr = __builtin_log(px / **opt);
                    out.push(lr);
                }
            }
            auto existing = prev_close.try_get(sv);
            if(existing.ok()) {
                **existing = px;
            } else {
                prev_close.insert(sb.symbol.clone(), f64{px});
            }
        }
        return out;
    }

    [[nodiscard]] Ohlcv_Data<A> resample(String_View target_freq) const noexcept;
};

} // namespace spp::quant::data

SPP_RECORD(spp::quant::data::Bar,
    SPP_FIELD(time), SPP_FIELD(open), SPP_FIELD(high),
    SPP_FIELD(low), SPP_FIELD(close), SPP_FIELD(volume));

SPP_RECORD(spp::quant::data::Symbol_Bar,
    SPP_FIELD(symbol), SPP_FIELD(bar));
