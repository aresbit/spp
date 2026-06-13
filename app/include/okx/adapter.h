#pragma once

// OKX <-> spp::quant data-layer adapters.
//
// `mcp::klines_to_ohlcv` does this for Binance; this mirrors it for OKX
// so the Chan strategy / backtest engine can ingest OKX candles unchanged.
//
// OKX returns most prices/sizes as strings (preserving precision) — we
// reuse the small `str_to_f64` already defined in the Binance adapter
// rather than re-implementing it here.

#include <spp/core/base.h>
#include <spp/core/deterministic.h>
#include <spp/containers/string0.h>
#include <spp/containers/vec.h>
#include <spp/quant/data/types.h>
#include <spp/quant/data/ohlcv_data.h>

#include <okx/models.h>

namespace spp::App::Okx {

namespace detail {

// Local copy of the Binance adapter's str→f64 (same algorithm, just kept
// local so this header has no cross-include into app/mcp/).
[[nodiscard]] inline f64 str_to_f64(String_View sv) noexcept {
    f64 whole = 0.0, frac = 0.0, div = 1.0;
    bool neg = false;
    u64 i = 0;
    if(i < sv.length() && sv[i] == '-') { neg = true; i++; }
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
    return neg ? -(whole + frac / div) : (whole + frac / div);
}

} // namespace detail

// Convert a single OKX Candlestick into a quant data::Bar. `open_time`
// is interpreted as ms since epoch (OKX returns it as a string of ms,
// already parsed by `parse_candles`).
[[nodiscard]] inline quant::data::Bar
candlestick_to_bar(const Candlestick& c) noexcept {
    quant::data::Bar bar;
    bar.time   = Deterministic_Time::from_unix_ms(c.open_time);
    bar.open   = quant::data::f64_to_price(detail::str_to_f64(c.open.view()));
    bar.high   = quant::data::f64_to_price(detail::str_to_f64(c.high.view()));
    bar.low    = quant::data::f64_to_price(detail::str_to_f64(c.low.view()));
    bar.close  = quant::data::f64_to_price(detail::str_to_f64(c.close.view()));
    bar.volume = detail::str_to_f64(c.vol.view());
    return bar;
}

// Convert a vector of OKX candlesticks to a single-symbol Ohlcv_Data.
//
// IMPORTANT: OKX returns candles NEWEST-FIRST (descending by time). We
// reverse iterate so the resulting Ohlcv_Data is in chronological order
// — that's what every downstream consumer (Chan, backtest engine,
// resampler) expects.
template<typename A = Mdefault>
[[nodiscard]] inline quant::data::Ohlcv_Data<A>
candles_to_ohlcv(String_View instId,
                 const Vec<Candlestick, Mdefault>& candles) noexcept {
    quant::data::Ohlcv_Data<A> out;
    if(candles.length() == 0) return out;
    for(u64 i = candles.length(); i > 0; i--) {
        const auto& c = candles[i - 1];
        quant::data::Symbol_Bar sb;
        sb.symbol = instId.template string<A>();
        sb.bar = candlestick_to_bar(c);
        out.bars.push(spp::move(sb));
    }
    return out;
}

} // namespace spp::App::Okx
