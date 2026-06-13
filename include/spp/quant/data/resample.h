#pragma once
#include <spp/core/base.h>
#include <spp/core/tuple.h>
#include <spp/quant/data/types.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/data/tick_data.h>

namespace spp::quant::data {

template <typename A>
struct Resampler {
    // Tick holds a String<> (non-copyable) so field-by-field copy + clone the
    // owning member.
    static void _copy_tick(Tick& dst, const Tick& src) noexcept {
        dst.time = src.time;
        dst.price = src.price;
        dst.volume = src.volume;
        dst.amount = src.amount;
        dst.direction = src.direction;
        dst.order_seq = src.order_seq;
        dst.code = src.code.clone();
    }

    static auto tick_to_bar(const Tick_Data<A>& ticks, String_View freq) -> Ohlcv_Data<A> {
        Ohlcv_Data<A> result;
        Frequency target_freq = parse_frequency(freq);
        Deterministic_Duration bucket_dur = frequency_duration(target_freq);
        i64 bucket_ns = bucket_dur.ns();
        result.frequency = freq.string<A>();
        result.market_type = "unknown"_v.string<A>();

        if(ticks.tick_count() == 0) return result;

        Vec<String<A>, A> sym_list;
        {
            Map<String<Mdefault>, u8, A> seen2;
            for(u64 i = 0; i < ticks.tick_count(); i++) {
                String_View sv = ticks.ticks[i].symbol.view();
                if(!seen2.contains(sv)) {
                    seen2.insert(ticks.ticks[i].symbol.clone(), 1);
                    sym_list.push(sv.string<A>());
                }
            }
        }

        for(u64 si = 0; si < sym_list.length(); si++) {
            String_View sym = sym_list[si].view();

            Map<Deterministic_Time, Vec<Symbol_Tick, A>, A> buckets;
            for(u64 i = 0; i < ticks.tick_count(); i++) {
                if(ticks.ticks[i].symbol != sym) continue;
                auto t = ticks.ticks[i].tick.time;
                i64 bucket_time_ns;
                if(bucket_ns == 0) {
                    bucket_time_ns = t.unix_ns();
                } else {
                    i64 ns = t.unix_ns();
                    bucket_time_ns = (ns / bucket_ns) * bucket_ns;
                }
                auto bucket_t = Deterministic_Time::from_unix_ns(bucket_time_ns);

                auto existing = buckets.try_get(bucket_t);
                if(existing.ok()) {
                    Symbol_Tick st;
                    st.symbol = ticks.ticks[i].symbol.clone();
                    _copy_tick(st.tick, ticks.ticks[i].tick);
                    (**existing).push(spp::move(st));
                } else {
                    Vec<Symbol_Tick, A> v;
                    Symbol_Tick st;
                    st.symbol = ticks.ticks[i].symbol.clone();
                    _copy_tick(st.tick, ticks.ticks[i].tick);
                    v.push(spp::move(st));
                    buckets.insert(Deterministic_Time::from_unix_ns(bucket_t.unix_ns()),
                                   spp::move(v));
                }
            }

            Vec<Deterministic_Time, A> sorted_times;
            for(u64 i = 0; i < ticks.tick_count(); i++) {
                if(ticks.ticks[i].symbol != sym) continue;
                auto t = ticks.ticks[i].tick.time;
                i64 bucket_time_ns;
                if(bucket_ns == 0) {
                    bucket_time_ns = t.unix_ns();
                } else {
                    i64 ns = t.unix_ns();
                    bucket_time_ns = (ns / bucket_ns) * bucket_ns;
                }
                auto bucket_t = Deterministic_Time::from_unix_ns(bucket_time_ns);

                bool found = false;
                for(u64 k = 0; k < sorted_times.length(); k++) {
                    if(sorted_times[k].unix_ns() == bucket_t.unix_ns()) {
                        found = true;
                        break;
                    }
                }
                if(!found) sorted_times.push(bucket_t);
            }

            for(u64 bi = 0; bi < sorted_times.length(); bi++) {
                auto bt = sorted_times[bi];
                auto opt_bucket = buckets.try_get(bt);
                if(!opt_bucket.ok()) continue;
                auto& bucket = **opt_bucket;
                if(bucket.length() == 0) continue;

                Symbol_Bar sb;
                sb.symbol = sym.template string<Mdefault>();
                sb.bar.time = bt;
                sb.bar.open = bucket[0].tick.price;
                sb.bar.high = bucket[0].tick.price;
                sb.bar.low = bucket[0].tick.price;
                sb.bar.close = bucket[bucket.length() - 1].tick.price;
                f64 vol = 0.0;
                for(u64 k = 0; k < bucket.length(); k++) {
                    if(bucket[k].tick.price > sb.bar.high) sb.bar.high = bucket[k].tick.price;
                    if(bucket[k].tick.price < sb.bar.low) sb.bar.low = bucket[k].tick.price;
                    vol += bucket[k].tick.volume;
                }
                sb.bar.volume = vol;
                result.bars.push(spp::move(sb));
            }
        }
        return result;
    }

    static auto bar_to_bar(const Ohlcv_Data<A>& bars, String_View target_freq) -> Ohlcv_Data<A> {
        Ohlcv_Data<A> result;
        Frequency tgt_freq = parse_frequency(target_freq);
        Deterministic_Duration tgt_dur = frequency_duration(tgt_freq);
        i64 bucket_ns = tgt_dur.ns();
        result.frequency = target_freq.string<A>();
        result.market_type = bars.market_type.clone();

        if(bars.bar_count() == 0 || bucket_ns == 0) return result;

        Vec<String<A>, A> sym_list;
        {
            Map<String<Mdefault>, u8, A> seen;
            for(u64 i = 0; i < bars.bars.length(); i++) {
                String_View sv = bars.bars[i].symbol.view();
                if(!seen.contains(sv)) {
                    seen.insert(bars.bars[i].symbol.clone(), 1);
                    sym_list.push(sv.string<A>());
                }
            }
        }

        for(u64 si = 0; si < sym_list.length(); si++) {
            String_View sym = sym_list[si].view();

            Map<Deterministic_Time, Vec<Symbol_Bar, A>, A> buckets;
            for(u64 i = 0; i < bars.bars.length(); i++) {
                if(bars.bars[i].symbol != sym) continue;
                auto t = bars.bars[i].bar.time;
                i64 ns = t.unix_ns();
                i64 bucket_time_ns = (ns / bucket_ns) * bucket_ns;
                auto bucket_t = Deterministic_Time::from_unix_ns(bucket_time_ns);

                auto existing = buckets.try_get(bucket_t);
                if(existing.ok()) {
                    Symbol_Bar sb;
                    sb.symbol = bars.bars[i].symbol.clone();
                    sb.bar = bars.bars[i].bar;
                    (**existing).push(spp::move(sb));
                } else {
                    Vec<Symbol_Bar, A> v;
                    Symbol_Bar sb;
                    sb.symbol = bars.bars[i].symbol.clone();
                    sb.bar = bars.bars[i].bar;
                    v.push(spp::move(sb));
                    buckets.insert(Deterministic_Time::from_unix_ns(bucket_t.unix_ns()),
                                   spp::move(v));
                }
            }

            Vec<Deterministic_Time, A> sorted_times;
            for(u64 i = 0; i < bars.bars.length(); i++) {
                if(bars.bars[i].symbol != sym) continue;
                auto t = bars.bars[i].bar.time;
                i64 ns = t.unix_ns();
                i64 bucket_time_ns = (ns / bucket_ns) * bucket_ns;
                auto bucket_t = Deterministic_Time::from_unix_ns(bucket_time_ns);

                bool found = false;
                for(u64 k = 0; k < sorted_times.length(); k++) {
                    if(sorted_times[k].unix_ns() == bucket_t.unix_ns()) {
                        found = true;
                        break;
                    }
                }
                if(!found) sorted_times.push(bucket_t);
            }

            for(u64 bi = 0; bi < sorted_times.length(); bi++) {
                auto bt = sorted_times[bi];
                auto opt_bucket = buckets.try_get(bt);
                if(!opt_bucket.ok()) continue;
                auto& bucket = **opt_bucket;
                if(bucket.length() == 0) continue;

                Symbol_Bar sb;
                sb.symbol = sym.template string<Mdefault>();
                sb.bar.time = bt;
                sb.bar.open = bucket[0].bar.open;
                sb.bar.high = bucket[0].bar.high;
                sb.bar.low = bucket[0].bar.low;
                sb.bar.close = bucket[bucket.length() - 1].bar.close;
                f64 vol = 0.0;
                for(u64 k = 0; k < bucket.length(); k++) {
                    if(bucket[k].bar.high > sb.bar.high) sb.bar.high = bucket[k].bar.high;
                    if(bucket[k].bar.low < sb.bar.low) sb.bar.low = bucket[k].bar.low;
                    vol += bucket[k].bar.volume;
                }
                sb.bar.volume = vol;
                result.bars.push(spp::move(sb));
            }
        }
        return result;
    }

    static auto bar_to_day(const Ohlcv_Data<A>& min_bars) -> Ohlcv_Data<A> {
        return bar_to_bar(min_bars, "1d"_v);
    }

    static auto day_to_week(const Ohlcv_Data<A>& day_bars) -> Ohlcv_Data<A> {
        return bar_to_bar(day_bars, "1w"_v);
    }

    static auto day_to_month(const Ohlcv_Data<A>& day_bars) -> Ohlcv_Data<A> {
        return bar_to_bar(day_bars, "1M"_v);
    }
};

template <typename A>
Ohlcv_Data<A> Ohlcv_Data<A>::resample(String_View target_freq) const noexcept {
    return Resampler<A>::bar_to_bar(*this, target_freq);
}

template <typename A>
Ohlcv_Data<A> Tick_Data<A>::resample_to_bars(String_View freq) const noexcept {
    return Resampler<A>::tick_to_bar(*this, freq);
}

template <typename A>
Tuple<u64, u64, u64> Tick_Data<A>::classify_orders() const noexcept {
    u64 big = 0, medium = 0, small = 0;
    if(ticks.length() == 0) return Tuple<u64, u64, u64>{big, medium, small};

    f64 total_vol = 0.0;
    for(u64 i = 0; i < ticks.length(); i++) {
        total_vol += ticks[i].tick.volume;
    }
    f64 avg_vol = total_vol / static_cast<f64>(ticks.length());

    f64 big_threshold = avg_vol * 3.0;
    f64 small_threshold = avg_vol * 0.5;

    for(u64 i = 0; i < ticks.length(); i++) {
        f64 v = ticks[i].tick.volume;
        if(v >= big_threshold) {
            big++;
        } else if(v >= small_threshold) {
            medium++;
        } else {
            small++;
        }
    }
    return Tuple<u64, u64, u64>{big, medium, small};
}

} // namespace spp::quant::data
