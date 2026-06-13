#pragma once

// Tick-to-bar aggregator for Binance Spot aggTrade WS frames.
//
// Wire-format examples (Spot, public stream):
//
//   {"e":"aggTrade","E":1700000000123,"s":"BTCUSDT",
//    "a":12345,"p":"42000.50","q":"0.001",
//    "f":11111,"l":11112,"T":1700000000000,"m":true,"M":true}
//
// We need exactly four fields: s (symbol), p (price), q (qty), T (trade
// timestamp ms). We extract them with a single forward scan rather than
// going through the full reflection-based JSON parser — the payload is
// small, the format is stable, and the live path needs to keep allocator
// pressure low.
//
// The aggregator buckets trades by `T / bucket_ms * bucket_ms` (trade
// time, NOT event time — the exchange's matching-engine timestamp is the
// canonical clock). When a trade arrives with a different bucket than the
// in-flight bar's, the in-flight bar is closed and the callback fires.
//
// Design notes:
//   - One aggregator per symbol. The live driver owns a `Map<symbol, Bar_Aggregator>`.
//   - Bars are emitted on the *next* trade after the bucket boundary —
//     i.e. a 12:00:00 bar fires when the first 12:01:00 trade arrives.
//     Idle markets will silently delay the close; callers that need an
//     absolute timer must poke `flush_if_due(now_ms)` from the event loop.
//   - Volume is the SUM of trade qty in the bucket. open/high/low/close
//     follow the standard OHLC rule.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/data/types.h>

namespace spp::App::Binance {

namespace detail {

// Forward-scan extractor: finds `"key":` and returns the string-value
// that follows (without the surrounding quotes). Numbers are returned
// as their literal text; quoted strings have their quotes stripped.
// Returns empty view if the key is missing.
[[nodiscard]] inline String_View
json_field_(String_View body, String_View key) noexcept {
    // Build the search pattern "<key>": as a temporary in a small stack
    // buffer (key length is bounded — Binance keys are <=8 chars).
    char pat[16];
    u64 plen = 0;
    if(key.length() + 3 >= sizeof(pat)) return ""_v;
    pat[plen++] = '"';
    for(u64 i = 0; i < key.length(); i++) pat[plen++] = static_cast<char>(key[i]);
    pat[plen++] = '"';
    pat[plen++] = ':';

    // Naive substring search — keys are unique within the small payload,
    // and the payload is ≤ ~200 bytes per aggTrade.
    for(u64 i = 0; i + plen <= body.length(); i++) {
        bool match = true;
        for(u64 j = 0; j < plen; j++) {
            if(body[i + j] != static_cast<u8>(pat[j])) { match = false; break; }
        }
        if(!match) continue;

        // Found the key. Skip optional whitespace, then read either a
        // quoted string or a number.
        u64 k = i + plen;
        while(k < body.length() && (body[k] == ' ' || body[k] == '\t')) k++;
        if(k >= body.length()) return ""_v;

        if(body[k] == '"') {
            u64 start = ++k;
            while(k < body.length() && body[k] != '"') k++;
            return String_View{body.data() + start, k - start};
        }
        // Number / bool / null — consume until separator.
        u64 start = k;
        while(k < body.length() && body[k] != ',' && body[k] != '}' &&
              body[k] != ']' && body[k] != ' ' && body[k] != '\t') k++;
        return String_View{body.data() + start, k - start};
    }
    return ""_v;
}

// Decimal parser sharing the same shape as mcp::str_to_f64 but kept
// local so this header has no MCP dependency.
[[nodiscard]] inline f64 parse_decimal_(String_View sv) noexcept {
    if(sv.length() == 0) return 0.0;
    f64 whole = 0.0, frac = 0.0, div = 1.0;
    bool neg = false;
    u64 i = 0;
    if(sv[i] == '-') { neg = true; i++; }
    while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
        whole = whole * 10.0 + static_cast<f64>(sv[i] - '0');
        i++;
    }
    if(i < sv.length() && sv[i] == '.') {
        i++;
        while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
            frac = frac * 10.0 + static_cast<f64>(sv[i] - '0');
            div *= 10.0;
            i++;
        }
    }
    f64 v = whole + frac / div;
    return neg ? -v : v;
}

[[nodiscard]] inline i64 parse_i64_(String_View sv) noexcept {
    i64 v = 0;
    bool neg = false;
    u64 i = 0;
    if(i < sv.length() && sv[i] == '-') { neg = true; i++; }
    while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
        v = v * 10 + static_cast<i64>(sv[i] - '0');
        i++;
    }
    return neg ? -v : v;
}

} // namespace detail

// One aggregator per symbol. Construct with the bar duration (60_000 for
// 1-minute) and a closure-style callback. Feed each aggTrade JSON via
// `on_message`. The callback fires synchronously when a bar closes.
template<typename On_Bar>
struct Bar_Aggregator {
    i64 bucket_ms = 60000;
    String<Mdefault> symbol;          // upper-case canonical (e.g. "BTCUSDT")
    On_Bar on_bar;                    // void(const quant::data::Symbol_Bar&)

    // In-flight bar state.
    i64 cur_bucket_start_ms = 0;
    bool in_flight = false;
    f64 open_ = 0.0, high_ = 0.0, low_ = 0.0, close_ = 0.0;
    f64 volume_ = 0.0;
    u64 trade_count_ = 0;

    template<typename U>
    Bar_Aggregator(String_View sym, i64 bucket, U&& cb) noexcept
        : bucket_ms(bucket > 0 ? bucket : 60000),
          symbol(sym.template string<Mdefault>()),
          on_bar(spp::forward<U>(cb)) {
    }

    // Ingest a single aggTrade JSON payload. Returns the bucket epoch the
    // trade landed in, or an error if mandatory fields couldn't be parsed.
    [[nodiscard]] Result<i64, String_View> on_message(String_View body) noexcept {
        auto sv_symbol = detail::json_field_(body, "s"_v);
        auto sv_price  = detail::json_field_(body, "p"_v);
        auto sv_qty    = detail::json_field_(body, "q"_v);
        auto sv_time   = detail::json_field_(body, "T"_v);
        if(sv_price.length() == 0 || sv_qty.length() == 0 ||
           sv_time.length() == 0) {
            return Result<i64, String_View>::err("aggTrade_missing_field"_v);
        }
        // Reject trades from other symbols — the callback should never see
        // mixed-symbol bars even if subscriptions overlap.
        if(sv_symbol.length() > 0 && sv_symbol != symbol.view()) {
            return Result<i64, String_View>::err("aggTrade_symbol_mismatch"_v);
        }
        f64 price = detail::parse_decimal_(sv_price);
        f64 qty = detail::parse_decimal_(sv_qty);
        i64 trade_ms = detail::parse_i64_(sv_time);
        i64 bucket_start = (trade_ms / bucket_ms) * bucket_ms;
        ingest_(bucket_start, price, qty);
        return Result<i64, String_View>::ok(spp::move(bucket_start));
    }

    // Wall-clock poke: if the in-flight bar's bucket has elapsed, close it
    // even though no trade has arrived. Useful for illiquid pairs or for
    // ensuring the strategy runs at least once per minute.
    void flush_if_due(i64 now_ms) noexcept {
        if(!in_flight) return;
        i64 next_bucket = cur_bucket_start_ms + bucket_ms;
        if(now_ms >= next_bucket) {
            close_bar_();
        }
    }

    // Force-close the in-flight bar (e.g. on disconnect). The caller is
    // responsible for handling the resulting partial-bar callback.
    void force_close() noexcept {
        if(in_flight) close_bar_();
    }

private:
    void ingest_(i64 bucket_start_ms, f64 price, f64 qty) noexcept {
        if(!in_flight) {
            start_bar_(bucket_start_ms, price);
        } else if(bucket_start_ms != cur_bucket_start_ms) {
            // Crossed a bucket boundary — close, then open new.
            close_bar_();
            start_bar_(bucket_start_ms, price);
        }
        // Update OHLC of the in-flight bar.
        if(price > high_) high_ = price;
        if(price < low_)  low_  = price;
        close_ = price;
        volume_ += qty;
        trade_count_++;
    }

    void start_bar_(i64 bucket_start_ms, f64 price) noexcept {
        cur_bucket_start_ms = bucket_start_ms;
        open_ = high_ = low_ = close_ = price;
        volume_ = 0.0;
        trade_count_ = 0;
        in_flight = true;
    }

    void close_bar_() noexcept {
        quant::data::Symbol_Bar sb;
        sb.symbol = symbol.clone();
        sb.bar.time = Deterministic_Time::from_unix_ms(cur_bucket_start_ms);
        sb.bar.open  = quant::data::f64_to_price(open_);
        sb.bar.high  = quant::data::f64_to_price(high_);
        sb.bar.low   = quant::data::f64_to_price(low_);
        sb.bar.close = quant::data::f64_to_price(close_);
        sb.bar.volume = volume_;
        on_bar(sb);
        in_flight = false;
    }
};

// Deduction guide so callers can write `Bar_Aggregator agg{sym, 60000, lambda};`.
template<typename On_Bar>
Bar_Aggregator(String_View, i64, On_Bar) -> Bar_Aggregator<On_Bar>;

} // namespace spp::App::Binance
