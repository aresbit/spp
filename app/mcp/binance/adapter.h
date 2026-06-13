#pragma once

// Thin convenience wrapper over spp::App::Binance::Client that adds
// klines→Ohlcv_Data conversion and the str→f64 parser used by MCP tools.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/core/deterministic.h>
#include <spp/containers/string0.h>
#include <spp/containers/vec.h>
#include <spp/quant/data/types.h>
#include <spp/quant/data/ohlcv_data.h>

#include <binance/client.h>
#include <binance/models.h>
#include <binance/api.h>
#include <binance/clock.h>

namespace spp::mcp {

// Simple str→f64 for Binance's string-encoded prices/quantities.
[[nodiscard]] inline f64 str_to_f64(String_View sv) noexcept {
    f64 result = 0.0, frac = 0.0, div = 1.0;
    bool neg = false;
    u64 i = 0;
    if (i < sv.length() && sv[i] == '-') { neg = true; i++; }
    while (i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
        result = result * 10.0 + (f64)(sv[i] - '0'); i++;
    }
    if (i < sv.length() && sv[i] == '.') {
        i++;
        while (i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
            frac = frac * 10.0 + (f64)(sv[i] - '0'); div *= 10.0; i++;
        }
    }
    return neg ? -(result + frac / div) : (result + frac / div);
}

// Convert a Binance Kline to a quant data::Bar.
[[nodiscard]] inline quant::data::Bar kline_to_bar(const App::Binance::Kline& k) noexcept {
    quant::data::Bar bar;
    bar.time   = Deterministic_Time::from_unix_ms(k.open_time);
    bar.open   = quant::data::f64_to_price(str_to_f64(k.open.view()));
    bar.high   = quant::data::f64_to_price(str_to_f64(k.high.view()));
    bar.low    = quant::data::f64_to_price(str_to_f64(k.low.view()));
    bar.close  = quant::data::f64_to_price(str_to_f64(k.close.view()));
    bar.volume = str_to_f64(k.volume.view());
    return bar;
}

// Convert klines vector → Ohlcv_Data (single symbol).
template <typename A = Mdefault>
[[nodiscard]] inline quant::data::Ohlcv_Data<A> klines_to_ohlcv(
    String_View symbol, const Vec<App::Binance::Kline, Mdefault>& klines) noexcept {
    quant::data::Ohlcv_Data<A> out;
    for (u64 i = 0; i < klines.length(); i++) {
        quant::data::Symbol_Bar sb;
        sb.symbol = symbol.template string<A>();
        sb.bar = kline_to_bar(klines[i]);
        out.bars.push(spp::move(sb));
    }
    return out;
}

// JSON-escaping helper for tool output (minimal — handles " and \).
[[nodiscard]] inline String<Mdefault> json_escape(String_View sv) noexcept {
    u64 extra = 0;
    for (u64 i = 0; i < sv.length(); i++) {
        if (sv[i] == '"' || sv[i] == '\\' || sv[i] == '\n') extra++;
    }
    String<Mdefault> out(sv.length() + extra + 3); // +3 for safety
    u64 w = 0;
    for (u64 i = 0; i < sv.length(); i++) {
        u8 c = sv[i];
        if (c == '"')  { out.data()[w++] = '\\'; out.data()[w++] = '"'; }
        else if (c == '\\') { out.data()[w++] = '\\'; out.data()[w++] = '\\'; }
        else if (c == '\n') { out.data()[w++] = '\\'; out.data()[w++] = 'n'; }
        else { out.data()[w++] = c; }
    }
    out.set_length(w);
    out.data()[w] = 0;
    return out;
}

// JSON-stringify helper using SPP reflection.
template <typename T>
[[nodiscard]] inline String<Mdefault> to_json(const T& val) noexcept {
    Json::Builder<Mdefault> b;
    Json::write_json(b, val);
    return b.build();
}

} // namespace spp::mcp
