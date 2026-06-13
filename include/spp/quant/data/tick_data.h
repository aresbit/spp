#pragma once
#include <spp/core/base.h>
#include <spp/core/tuple.h>
#include <spp/quant/data/types.h>
#include <spp/quant/data/ohlcv_data.h>

namespace spp::quant::data {

struct Tick {
    Deterministic_Time time;
    Decimal<8> price;
    f64 volume;
    f64 amount;
    u8 direction;
    u64 order_seq;
    String<Mdefault> code;
};

struct Symbol_Tick {
    String<Mdefault> symbol;
    Tick tick;
};

template <typename A = Mdefault>
struct Tick_Data {
    Vec<Symbol_Tick, A> ticks;

    Tick_Data() noexcept = default;
    Tick_Data(Tick_Data&&) noexcept = default;
    Tick_Data& operator=(Tick_Data&&) noexcept = default;

    [[nodiscard]] u64 tick_count() const noexcept {
        return ticks.length();
    }

    [[nodiscard]] u64 symbol_count() const noexcept {
        Map<String<Mdefault>, u8, A> seen;
        for(u64 i = 0; i < ticks.length(); i++) {
            String_View sv = ticks[i].symbol.view();
            if(!seen.contains(sv)) {
                seen.insert(ticks[i].symbol.clone(), 1);
            }
        }
        return seen.length();
    }

    [[nodiscard]] Vec<String<A>, A> symbols() const noexcept {
        Map<String<Mdefault>, u8, A> seen;
        Vec<String<A>, A> out;
        for(u64 i = 0; i < ticks.length(); i++) {
            String_View sv = ticks[i].symbol.view();
            if(!seen.contains(sv)) {
                seen.insert(ticks[i].symbol.clone(), 1);
                out.push(sv.template string<A>());
            }
        }
        return out;
    }

    [[nodiscard]] Tick_Data<A> select_code(String_View code) const noexcept {
        Tick_Data<A> result;
        for(u64 i = 0; i < ticks.length(); i++) {
            if(ticks[i].symbol == code) {
                Symbol_Tick st;
                st.symbol = ticks[i].symbol.clone();
                _copy_tick(st.tick, ticks[i].tick);
                result.ticks.push(spp::move(st));
            }
        }
        return result;
    }

    [[nodiscard]] Tick_Data<A> select_time(Deterministic_Time start,
                                           Deterministic_Time end) const noexcept {
        Tick_Data<A> result;
        for(u64 i = 0; i < ticks.length(); i++) {
            auto t = ticks[i].tick.time;
            if(!(t < start) && t < end) {
                Symbol_Tick st;
                st.symbol = ticks[i].symbol.clone();
                _copy_tick(st.tick, ticks[i].tick);
                result.ticks.push(spp::move(st));
            }
        }
        return result;
    }

private:
    // Tick holds a String (non-copyable) so we can't `dst = src`. Clone the
    // owning members explicitly. Trivial members copy via plain assignment.
    static void _copy_tick(Tick& dst, const Tick& src) noexcept {
        dst.time = src.time;
        dst.price = src.price;
        dst.volume = src.volume;
        dst.amount = src.amount;
        dst.direction = src.direction;
        dst.order_seq = src.order_seq;
        dst.code = src.code.clone();
    }

public:

    [[nodiscard]] Vec<f64, A> prices() const noexcept {
        Vec<f64, A> out;
        for(u64 i = 0; i < ticks.length(); i++) {
            out.push(price_to_f64(ticks[i].tick.price));
        }
        return out;
    }

    [[nodiscard]] Ohlcv_Data<A> resample_to_bars(String_View freq) const noexcept;

    [[nodiscard]] Tuple<u64, u64, u64> classify_orders() const noexcept;
};

} // namespace spp::quant::data

SPP_RECORD(spp::quant::data::Tick,
    SPP_FIELD(time), SPP_FIELD(price), SPP_FIELD(volume),
    SPP_FIELD(amount), SPP_FIELD(direction), SPP_FIELD(order_seq),
    SPP_FIELD(code));

SPP_RECORD(spp::quant::data::Symbol_Tick,
    SPP_FIELD(symbol), SPP_FIELD(tick));
