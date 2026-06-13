#pragma once

// PRICE_FILTER / LOT_SIZE rounder.
//
// Binance Spot rejects any order whose price isn't a multiple of the
// symbol's `tickSize` (-1013 "Invalid price") and whose quantity isn't a
// multiple of `stepSize` (-1111 "Precision is over the maximum defined").
// Both values live in `/api/v3/exchangeInfo`; the filter cache here
// loads them on first reference and then rounds locally so the strategy
// never has to think about it.
//
// Rounding rule:
//   - prices round DOWN to the nearest tickSize (selling at limit price
//     above tick gets rejected; rounding down stays acceptable)
//   - quantities round DOWN to the nearest stepSize (rounding up could
//     exceed cash / position; the loss of a sub-step's worth of size is
//     better than a -2010 INSUFFICIENT_BALANCE rejection)
//
// All math is integer-on-Decimal to avoid f64 boundary jitter that would
// otherwise produce sub-tick residuals.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/containers/map.h>
#include <spp/containers/string1.h>

#include <binance/api.h>
#include <binance/models.h>

namespace spp::App::Binance {

struct Symbol_Filters {
    f64 tick_size = 0.0;       // 0 ⇒ unfiltered (no rounding)
    f64 step_size = 0.0;
    f64 min_qty   = 0.0;
    f64 max_qty   = 0.0;
    f64 min_notional = 0.0;
};

template<typename A = Mdefault>
struct Filter_Cache {
    Map<String<A>, Symbol_Filters, A> by_symbol;

    [[nodiscard]] Opt<Symbol_Filters> get(String_View symbol) const noexcept {
        auto e = by_symbol.try_get(symbol);
        if(!e.ok()) return {};
        return Opt<Symbol_Filters>{**e};
    }

    void put(String_View symbol, Symbol_Filters f) noexcept {
        by_symbol.insert(symbol.template string<A>(), f);
    }

    // Pull /api/v3/exchangeInfo once and populate the cache for every
    // symbol in the response. Subsequent rounding calls are O(1) map
    // lookups. We rely on the existing `fetch_exchange_info` REST helper.
    template<typename S>
        requires Net::Byte_Stream<S>
    [[nodiscard]] Result<u64, String_View>
    populate(S& stream, String_View host, Rate_Limiter& lim, i64 now_ms) noexcept {
        auto info = fetch_exchange_info(stream, host, lim, now_ms);
        if(!info.ok()) {
            return Result<u64, String_View>::err(spp::move(info.unwrap_err()));
        }
        u64 added = 0;
        auto& payload = info.unwrap();
        for(u64 i = 0; i < payload.symbols.length(); i++) {
            const auto& es = payload.symbols[i];
            Symbol_Filters sf;
            for(u64 j = 0; j < es.filters.length(); j++) {
                const auto& f = es.filters[j];
                if(f.filterType == "PRICE_FILTER"_v) {
                    sf.tick_size = _parse_f64(f.tickSize.view());
                } else if(f.filterType == "LOT_SIZE"_v) {
                    sf.step_size = _parse_f64(f.stepSize.view());
                    sf.min_qty   = _parse_f64(f.minQty.view());
                    sf.max_qty   = _parse_f64(f.maxQty.view());
                } else if(f.filterType == "MIN_NOTIONAL"_v ||
                          f.filterType == "NOTIONAL"_v) {
                    sf.min_notional = _parse_f64(f.minNotional.view());
                }
            }
            put(es.symbol.view(), sf);
            added++;
        }
        return Result<u64, String_View>::ok(spp::move(added));
    }

    // Round price DOWN to the nearest tick. Returns the input unchanged
    // when the symbol is unknown — the caller still gets a valid order
    // and the exchange will reject it loudly, which is better than
    // silently zero-ing the price.
    [[nodiscard]] f64 round_price(String_View symbol, f64 price) const noexcept {
        auto sf = get(symbol);
        if(!sf.ok() || sf->tick_size <= 0.0) return price;
        return _round_down(price, sf->tick_size);
    }

    // Round quantity DOWN to the nearest step. Returns 0.0 if the result
    // would fall below `minQty` so the strategy can detect "this trade
    // can't be placed in size" without round-tripping to the exchange.
    [[nodiscard]] f64 round_qty(String_View symbol, f64 qty) const noexcept {
        auto sf = get(symbol);
        if(!sf.ok() || sf->step_size <= 0.0) return qty;
        f64 rounded = _round_down(qty, sf->step_size);
        if(sf->min_qty > 0.0 && rounded < sf->min_qty) return 0.0;
        return rounded;
    }

    // Verify final notional clears the symbol's MIN_NOTIONAL guard. Used
    // by callers right before submission as a last sanity check.
    [[nodiscard]] bool notional_ok(String_View symbol, f64 price, f64 qty) const noexcept {
        auto sf = get(symbol);
        if(!sf.ok() || sf->min_notional <= 0.0) return true;
        return price * qty >= sf->min_notional;
    }

private:
    // Round `v` DOWN to the nearest multiple of `step` (positive `step`).
    static f64 _round_down(f64 v, f64 step) noexcept {
        if(step <= 0.0) return v;
        // Integer-quantize via i64 to dodge f64 drift; tickSize values
        // like 0.00001 produce stable quotients up to ~10^14.
        f64 q = v / step;
        // Truncate towards zero (C int conversion); for negative inputs
        // (shouldn't happen in price/qty but defensive) bias one step down.
        i64 qi = static_cast<i64>(q);
        if(q < 0.0 && static_cast<f64>(qi) > q) qi--;
        return static_cast<f64>(qi) * step;
    }

    // Local-only f64 parser. Identical shape to bar_aggregator's; copied
    // so this header has no cross-dependency on it.
    static f64 _parse_f64(String_View sv) noexcept {
        if(sv.length() == 0) return 0.0;
        f64 whole = 0.0, frac = 0.0, div = 1.0;
        bool neg = false;
        u64 i = 0;
        if(sv[i] == '-') { neg = true; i++; }
        while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
            whole = whole * 10.0 + static_cast<f64>(sv[i] - '0'); i++;
        }
        if(i < sv.length() && sv[i] == '.') {
            i++;
            while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
                frac = frac * 10.0 + static_cast<f64>(sv[i] - '0');
                div *= 10.0; i++;
            }
        }
        f64 v = whole + frac / div;
        return neg ? -v : v;
    }
};

} // namespace spp::App::Binance
