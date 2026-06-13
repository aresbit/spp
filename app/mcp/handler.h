#pragma once

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/containers/string0.h>
#include <spp/containers/map.h>
#include <spp/reflection/json.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/factor/chan_fractal.h>

#include <binance/client.h>
#include <binance/models.h>
#include <mcp/codec/jsonrpc_codec.h>
#include <mcp/binance/adapter.h>
#include <okx/adapter.h>
#include <okx/client.h>
#include <okx/models.h>

// MCP_Handler — JSON-RPC dispatch engine for the spp-quant MCP server.
//
// Registers 10 tools covering market data, trading, and analysis.
// Each tool handler receives a raw JSON params string and returns
// a JSON result string (already escaped for embedding in MCP response).

namespace spp::mcp {

// Tool handler: takes raw JSON params string, returns JSON result string.
using Tool_Fn = Result<String<Mdefault>, String_View> (*)(
    App::Binance::Client& client, String_View params_json) noexcept;

// OKX tool handlers take an OKX client. Kept in a separate map keyed by
// "okx_*" tool names — the dispatcher routes by prefix.
using Okx_Tool_Fn = Result<String<Mdefault>, String_View> (*)(
    App::Okx::Client& client, String_View params_json) noexcept;

// Tool descriptor for tools/list response.
struct Tool_Def {
    String_View name;
    String_View description;
    String_View input_schema_json; // JSON Schema as raw JSON string
};

// ============================================================
// Tool parameter structs (parsed via SPP reflection from JSON)
// ============================================================
namespace tools {

struct Symbol_Params { String<Mdefault> symbol; };
struct Klines_Params {
    String<Mdefault> symbol;
    String<Mdefault> interval;
    u64 limit = 500;
    Opt<i64> startTime;
    Opt<i64> endTime;
};
struct Cancel_Params { String<Mdefault> symbol; i64 orderId = 0; };
struct Order_Params {
    String<Mdefault> symbol;
    String<Mdefault> side;       // BUY or SELL
    String<Mdefault> type;       // LIMIT or MARKET
    String<Mdefault> quantity;
    Opt<String<Mdefault>> price;
};
struct Chan_Params { String<Mdefault> symbol; String<Mdefault> interval; u64 limit = 200; };
struct Backtest_Params {
    String<Mdefault> symbol;
    String<Mdefault> interval;
    f64 init_cash = 10000.0;
    u64 limit = 500;
};

// OKX uses `instId` (e.g. "BTC-USDT") and `bar` (e.g. "1m") instead of
// Binance's symbol/interval. Keep them as separate param structs so the
// MCP schema reflects the wire names users expect.
struct Okx_Symbol_Params { String<Mdefault> instId; };
struct Okx_Candles_Params {
    String<Mdefault> instId;
    String<Mdefault> bar;
    u64 limit = 100;
};
struct Okx_Chan_Params {
    String<Mdefault> instId;
    String<Mdefault> bar;
    u64 limit = 200;
};
struct Okx_Order_Params {
    String<Mdefault> instId;
    String<Mdefault> side;     // "buy" / "sell"
    String<Mdefault> ordType;  // "limit" / "market"
    String<Mdefault> sz;
    Opt<String<Mdefault>> px;
};

} // namespace tools

struct Call_Params { String<Mdefault> name; Opt<String<Mdefault>> arguments; };

// ============================================================
// Tool implementations
// ============================================================

// --- Market Data Tools ---

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_get_server_time(App::Binance::Client& client, String_View) noexcept {
    auto st = client.server_time();
    if (!st.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(st.unwrap_err()));
    i64 local = App::Binance::now_ms();
    char buf[256];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"serverTime\":%lld,\"localTimeMs\":%lld,\"skewMs\":%lld}",
        (long long)st.unwrap().serverTime, (long long)local,
        (long long)(local - st.unwrap().serverTime));
    String<Mdefault> s((u64)n + 1);
    s.set_length((u64)n); Libc::memcpy(s.data(), buf, (u64)n); s.data()[n] = 0;
    return Result<String<Mdefault>, String_View>::ok(spp::move(s));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_get_ticker(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Symbol_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto bt = client.book_ticker(p.unwrap().symbol.view());
    if (!bt.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(bt.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(bt.unwrap()));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_get_klines(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Klines_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();
    auto kr = client.klines(pp.symbol.view(), pp.interval.view(), pp.limit,
                            pp.startTime, pp.endTime);
    if (!kr.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(kr.unwrap_err()));
    // Serialize klines as compact JSON
    auto& klines = kr.unwrap();
    Json::Builder<Mdefault> b;
    b.push('[');
    for (u64 i = 0; i < klines.length(); i++) {
        if (i > 0) b.push(',');
        auto& k = klines[i];
        char buf[512];
        i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
            "{\"t\":%lld,\"o\":\"%.*s\",\"h\":\"%.*s\",\"l\":\"%.*s\","
            "\"c\":\"%.*s\",\"v\":\"%.*s\"}",
            (long long)k.open_time,
            (i32)k.open.length(), k.open.data(),
            (i32)k.high.length(), k.high.data(),
            (i32)k.low.length(), k.low.data(),
            (i32)k.close.length(), k.close.data(),
            (i32)k.volume.length(), k.volume.data());
        b.append(String_View{(const u8*)buf, (u64)n});
    }
    b.push(']');
    return Result<String<Mdefault>, String_View>::ok(b.build());
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_get_order_book(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Symbol_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto bt = client.book_ticker(p.unwrap().symbol.view());
    if (!bt.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(bt.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(bt.unwrap()));
}

// --- Trading Tools ---

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_get_account(App::Binance::Client& client, String_View) noexcept {
    auto ai = client.account();
    if (!ai.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(ai.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(ai.unwrap()));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_place_order(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Order_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();
    App::Binance::Order_Request req;
    req.symbol = pp.symbol.view();
    req.side = pp.side.view() == "SELL"_v ? App::Binance::Side::SELL : App::Binance::Side::BUY;
    req.type = pp.type.view() == "MARKET"_v ? App::Binance::Order_Type::MARKET : App::Binance::Order_Type::LIMIT;
    req.quantity = pp.quantity.view();
    if (pp.price.ok()) req.price = (*pp.price).view();
    auto orr = client.place_order(req);
    if (!orr.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(orr.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(orr.unwrap()));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_cancel_order(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Cancel_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();
    auto os = client.cancel_order(pp.symbol.view(), pp.orderId);
    if (!os.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(os.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(os.unwrap()));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_query_order(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Cancel_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();
    auto os = client.query_order(pp.symbol.view(), pp.orderId);
    if (!os.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(os.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(os.unwrap()));
}

// --- Analysis Tools ---

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_run_chan_analysis(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Chan_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();

    // Fetch klines
    auto kr = client.klines(pp.symbol.view(), pp.interval.view(), pp.limit);
    if (!kr.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(kr.unwrap_err()));

    // Convert to Ohlcv_Data
    auto ohlcv = klines_to_ohlcv<Mdefault>(pp.symbol.view(), kr.unwrap());
    ohlcv.frequency = spp::move(pp.interval);

    // Run Chan analysis
    auto features = quant::factor::Chan_Features<Mdefault>::extract_all(ohlcv);
    auto fractals = quant::factor::Chan_Features<Mdefault>::detect_fractals(ohlcv, 5);

    // Build output JSON manually (avoid deep nested reflection for large Vecs)
    char buf[4096];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"symbol\":\"%.*s\",\"interval\":\"%.*s\","
        "\"bar_count\":%llu,\"fractal_count\":%llu,\"feature_count\":%llu}",
        (i32)pp.symbol.length(), pp.symbol.data(),
        (i32)pp.interval.length(), pp.interval.data(),
        (unsigned long long)ohlcv.bars.length(),
        (unsigned long long)fractals.length(),
        (unsigned long long)features.length());
    String<Mdefault> out((u64)n + 1);
    out.set_length((u64)n); Libc::memcpy(out.data(), buf, (u64)n); out.data()[n] = 0;
    return Result<String<Mdefault>, String_View>::ok(spp::move(out));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_run_backtest(App::Binance::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Backtest_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();

    // Fetch historical klines
    auto kr = client.klines(pp.symbol.view(), pp.interval.view(), pp.limit);
    if (!kr.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(kr.unwrap_err()));

    auto ohlcv = klines_to_ohlcv<Mdefault>(pp.symbol.view(), kr.unwrap());
    ohlcv.frequency = spp::move(pp.interval);

    if (ohlcv.bars.length() == 0) {
        return Result<String<Mdefault>, String_View>::err("no_data"_v);
    }

    // Buy-and-hold backtest: enter at the first bar's open, mark to the last
    // bar's close. Guard against a zero entry price (bad/empty data).
    auto& first = ohlcv.bars[0].bar;
    auto& last  = ohlcv.bars[ohlcv.bars.length() - 1].bar;
    f64 entry = quant::data::price_to_f64(first.open);
    f64 exit_ = quant::data::price_to_f64(last.close);
    f64 ret   = entry > 0.0 ? (exit_ - entry) / entry : 0.0;
    char buf[512];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"symbol\":\"%.*s\",\"bars\":%llu,\"entry\":%.8f,\"exit\":%.8f,"
        "\"buy_hold_return\":%.6f,\"note\":\"backtest_mvp\"}",
        (i32)pp.symbol.length(), pp.symbol.data(),
        (unsigned long long)ohlcv.bars.length(),
        entry, exit_, ret);
    String<Mdefault> out((u64)n + 1);
    out.set_length((u64)n); Libc::memcpy(out.data(), buf, (u64)n); out.data()[n] = 0;
    return Result<String<Mdefault>, String_View>::ok(spp::move(out));
}

// --- OKX Tools ---
// All follow the same shape: parse params via reflection, call client,
// re-serialise the typed response.  The Binance-side tools and these
// share the JSON-builder helpers (to_json, json_escape) defined in
// mcp/binance/adapter.h.

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_okx_get_ticker(App::Okx::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Okx_Symbol_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto r = client.ticker(p.unwrap().instId.view());
    if (!r.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(r.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(r.unwrap()));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_okx_get_balance(App::Okx::Client& client, String_View) noexcept {
    auto r = client.balance();
    if (!r.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(r.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(r.unwrap()));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_okx_get_candles(App::Okx::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Okx_Candles_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();
    auto cr = client.candles(pp.instId.view(), pp.bar.view(),
                              static_cast<u32>(pp.limit));
    if (!cr.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(cr.unwrap_err()));
    auto& cs = cr.unwrap();
    // Compact summary (skip the full array — clients can re-query if
    // they need raw bars).
    char buf[512];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"instId\":\"%.*s\",\"bar\":\"%.*s\",\"candles_count\":%llu}",
        (i32)pp.instId.length(), pp.instId.data(),
        (i32)pp.bar.length(), pp.bar.data(),
        (unsigned long long)cs.length());
    String<Mdefault> out((u64)n + 1);
    out.set_length((u64)n); Libc::memcpy(out.data(), buf, (u64)n); out.data()[n] = 0;
    return Result<String<Mdefault>, String_View>::ok(spp::move(out));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_okx_run_chan_analysis(App::Okx::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Okx_Chan_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();
    auto cr = client.candles(pp.instId.view(), pp.bar.view(),
                              static_cast<u32>(pp.limit));
    if (!cr.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(cr.unwrap_err()));

    auto ohlcv = App::Okx::candles_to_ohlcv<Mdefault>(
        pp.instId.view(), cr.unwrap());
    ohlcv.frequency = pp.bar.clone();

    auto features = quant::factor::Chan_Features<Mdefault>::extract_all(ohlcv);
    auto fractals = quant::factor::Chan_Features<Mdefault>::detect_fractals(ohlcv, 5);

    char buf[512];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"instId\":\"%.*s\",\"bar\":\"%.*s\","
        "\"bar_count\":%llu,\"fractal_count\":%llu,\"feature_count\":%llu}",
        (i32)pp.instId.length(), pp.instId.data(),
        (i32)pp.bar.length(), pp.bar.data(),
        (unsigned long long)ohlcv.bars.length(),
        (unsigned long long)fractals.length(),
        (unsigned long long)features.length());
    String<Mdefault> out((u64)n + 1);
    out.set_length((u64)n); Libc::memcpy(out.data(), buf, (u64)n); out.data()[n] = 0;
    return Result<String<Mdefault>, String_View>::ok(spp::move(out));
}

[[nodiscard]] inline Result<String<Mdefault>, String_View>
tool_okx_place_order(App::Okx::Client& client, String_View params_json) noexcept {
    auto p = Json::parse_result<tools::Okx_Order_Params>(params_json);
    if (!p.ok()) return Result<String<Mdefault>, String_View>::err("invalid_params"_v);
    auto& pp = p.unwrap();

    App::Okx::Order_Request req;
    req.instId  = pp.instId.view();
    req.tdMode  = App::Okx::Td_Mode::cash;
    req.side    = pp.side == "sell"_v ? App::Okx::Side::sell : App::Okx::Side::buy;
    req.ordType = pp.ordType == "market"_v
        ? App::Okx::Ord_Type::market : App::Okx::Ord_Type::limit;
    req.sz      = pp.sz.view();
    if (pp.px.ok()) req.px = (*pp.px).view();

    auto r = client.place_order(req);
    if (!r.ok()) return Result<String<Mdefault>, String_View>::err(spp::move(r.unwrap_err()));
    return Result<String<Mdefault>, String_View>::ok(to_json(r.unwrap()));
}

// ============================================================
// Handler: registers tools and dispatches JSON-RPC methods
// ============================================================
struct MCP_Handler {
    App::Binance::Client* client = null;
    App::Okx::Client* okx_client = null;
    Map<String<Mdefault>, Tool_Fn, Mdefault> tools;
    Map<String<Mdefault>, Okx_Tool_Fn, Mdefault> okx_tools;
    String<Mdefault> tools_json; // pre-built tools/list JSON fragment

    MCP_Handler() noexcept {
        register_tools();
    }

    void set_client(App::Binance::Client* c) noexcept { client = c; }
    void set_okx_client(App::Okx::Client* c) noexcept { okx_client = c; }

    void register_tools() noexcept {
        // Build the tools catalog for tools/list
        static const String_View tool_defs[] = {
            R"({"name":"get_server_time","description":"Get Binance server time and clock skew",)"_v
            R"("inputSchema":{"type":"object","properties":{}}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"get_ticker","description":"Get best bid/ask for a symbol",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"}},"required":["symbol"]}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"get_klines","description":"Get OHLCV candlestick data",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"},"interval":{"type":"string"},"limit":{"type":"integer"}},"required":["symbol","interval"]}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"get_order_book","description":"Get current order book best bid/ask",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"}},"required":["symbol"]}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"get_account","description":"Get account balances and trading status",)"_v
            R"("inputSchema":{"type":"object","properties":{}}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"place_order","description":"Place a LIMIT or MARKET order",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"},"side":{"type":"string"},"type":{"type":"string"},"quantity":{"type":"string"},"price":{"type":"string"}},"required":["symbol","side","type","quantity"]}},"_v
            R"("annotations":{"destructiveHint":true,"idempotentHint":false}})"_v,

            R"({"name":"cancel_order","description":"Cancel an open order by ID",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"},"orderId":{"type":"integer"}},"required":["symbol","orderId"]}},"_v
            R"("annotations":{"destructiveHint":true,"idempotentHint":true}})"_v,

            R"({"name":"query_order","description":"Query order status by ID",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"},"orderId":{"type":"integer"}},"required":["symbol","orderId"]}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"run_chan_analysis","description":"Run Chan Theory analysis: fractals, strokes, segments, pivots, trade points, divergence",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"},"interval":{"type":"string"},"limit":{"type":"integer"}},"required":["symbol","interval"]}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            R"({"name":"run_backtest","description":"Run a simple backtest on historical klines",)"_v
            R"("inputSchema":{"type":"object","properties":{"symbol":{"type":"string"},"interval":{"type":"string"},"init_cash":{"type":"number"},"limit":{"type":"integer"}},"required":["symbol","interval"]}},"_v
            R"("annotations":{"readOnlyHint":true}})"_v,

            // --- OKX tools ---
            // Use the `d(...)d` raw-string delimiter so parenthesised
            // examples inside `description` don't terminate the literal.
            R"d({"name":"okx_get_ticker","description":"Get OKX best bid/ask for an instId (e.g. BTC-USDT)",)d"_v
            R"d("inputSchema":{"type":"object","properties":{"instId":{"type":"string"}},"required":["instId"]},)d"_v
            R"d("annotations":{"readOnlyHint":true}})d"_v,

            R"d({"name":"okx_get_balance","description":"Get OKX account balances",)d"_v
            R"d("inputSchema":{"type":"object","properties":{}},)d"_v
            R"d("annotations":{"readOnlyHint":true}})d"_v,

            R"d({"name":"okx_get_candles","description":"Get OKX OHLCV candles. bar = 1m|5m|1H|...",)d"_v
            R"d("inputSchema":{"type":"object","properties":{"instId":{"type":"string"},"bar":{"type":"string"},"limit":{"type":"integer"}},"required":["instId","bar"]},)d"_v
            R"d("annotations":{"readOnlyHint":true}})d"_v,

            R"d({"name":"okx_run_chan_analysis","description":"Run Chan analysis on OKX candles",)d"_v
            R"d("inputSchema":{"type":"object","properties":{"instId":{"type":"string"},"bar":{"type":"string"},"limit":{"type":"integer"}},"required":["instId","bar"]},)d"_v
            R"d("annotations":{"readOnlyHint":true}})d"_v,

            R"d({"name":"okx_place_order","description":"Place a LIMIT or MARKET order on OKX (Spot, cash mode)",)d"_v
            R"d("inputSchema":{"type":"object","properties":{"instId":{"type":"string"},"side":{"type":"string"},"ordType":{"type":"string"},"sz":{"type":"string"},"px":{"type":"string"}},"required":["instId","side","ordType","sz"]},)d"_v
            R"d("annotations":{"destructiveHint":true,"idempotentHint":false}})d"_v,
        };

        // Build comma-joined tools JSON
        Json::Builder<Mdefault> tb;
        constexpr u64 n_defs = sizeof(tool_defs) / sizeof(tool_defs[0]);
        for (u64 i = 0; i < n_defs; i++) {
            if (i > 0) tb.push(',');
            tb.append(tool_defs[i]);
        }
        tools_json = tb.build();

        // Register tool function pointers
        tools.insert("get_server_time"_v.string<Mdefault>(), tool_get_server_time);
        tools.insert("get_ticker"_v.string<Mdefault>(), tool_get_ticker);
        tools.insert("get_klines"_v.string<Mdefault>(), tool_get_klines);
        tools.insert("get_order_book"_v.string<Mdefault>(), tool_get_order_book);
        tools.insert("get_account"_v.string<Mdefault>(), tool_get_account);
        tools.insert("place_order"_v.string<Mdefault>(), tool_place_order);
        tools.insert("cancel_order"_v.string<Mdefault>(), tool_cancel_order);
        tools.insert("query_order"_v.string<Mdefault>(), tool_query_order);
        tools.insert("run_chan_analysis"_v.string<Mdefault>(), tool_run_chan_analysis);
        tools.insert("run_backtest"_v.string<Mdefault>(), tool_run_backtest);

        // OKX tools live in a separate map so the dispatcher can route
        // them to `okx_client` instead of `client`.
        okx_tools.insert("okx_get_ticker"_v.string<Mdefault>(),       tool_okx_get_ticker);
        okx_tools.insert("okx_get_balance"_v.string<Mdefault>(),      tool_okx_get_balance);
        okx_tools.insert("okx_get_candles"_v.string<Mdefault>(),      tool_okx_get_candles);
        okx_tools.insert("okx_run_chan_analysis"_v.string<Mdefault>(), tool_okx_run_chan_analysis);
        okx_tools.insert("okx_place_order"_v.string<Mdefault>(),      tool_okx_place_order);
    }

    // Main dispatch: get the JSON-RPC response for a parsed Message_In.
    [[nodiscard]] String<Mdefault> process(const Message_In& msg) noexcept {
        auto type = classify(msg);
        if (type != Msg_Type::request) {
            return make_error_response(msg.id.ok() ? *msg.id : 0,
                                       MCP_INVALID_REQUEST, "not_a_request"_v);
        }
        u64 id = msg.id.ok() ? *msg.id : 0;
        String_View method = msg.method.ok() ? (*msg.method).view() : ""_v;

        // MCP lifecycle methods
        if (method == "initialize"_v) return make_init_response(id);
        if (method == "tools/list"_v) return make_tools_list_response(id, tools_json.view());

        if (method == "tools/call"_v) {
            if (!msg.params.ok()) return make_error_response(id, MCP_INVALID_PARAMS, "missing_params"_v);
            // Parse params to extract name and arguments
            auto cp = Json::parse_result<Call_Params>((*msg.params).view());
            if (!cp.ok()) return make_error_response(id, MCP_INVALID_PARAMS, "bad_params"_v);

            auto&& cp_val = cp.unwrap();
            String_View tool_name = cp_val.name.view();
            String_View args = cp_val.arguments.ok()
                ? (*cp_val.arguments).view() : "{}"_v;

            // OKX tools first — exclusive `okx_` prefix means we can
            // route by name lookup without ambiguity.
            auto okx_entry = okx_tools.try_get(tool_name);
            if (okx_entry.ok()) {
                if (okx_client == null) {
                    return make_error_response(id, MCP_INTERNAL_ERROR, "no_okx_client"_v);
                }
                auto result = (**okx_entry)(*okx_client, args);
                if (!result.ok()) {
                    return make_error_response(id, MCP_INTERNAL_ERROR, result.unwrap_err());
                }
                return make_tool_result(id, result.unwrap().view());
            }

            auto fn_entry = tools.try_get(tool_name);
            if (!fn_entry.ok()) {
                return make_error_response(id, MCP_TOOL_NOT_FOUND, "unknown_tool"_v);
            }
            if (client == null) {
                return make_error_response(id, MCP_INTERNAL_ERROR, "no_client"_v);
            }
            auto result = (**fn_entry)(*client, args);
            if (!result.ok()) {
                return make_error_response(id, MCP_INTERNAL_ERROR, result.unwrap_err());
            }
            return make_tool_result(id, result.unwrap().view());
        }

        return make_error_response(id, MCP_METHOD_NOT_FOUND, "method_not_found"_v);
    }
};

} // namespace spp::mcp

// === SPP_RECORD for tool params ===
namespace spp {

SPP_NAMED_RECORD(mcp::tools::Symbol_Params, "Symbol_Params", SPP_FIELD(symbol));
SPP_NAMED_RECORD(mcp::tools::Klines_Params, "Klines_Params",
    SPP_FIELD(symbol), SPP_FIELD(interval), SPP_FIELD(limit),
    SPP_FIELD(startTime), SPP_FIELD(endTime));
SPP_NAMED_RECORD(mcp::tools::Cancel_Params, "Cancel_Params",
    SPP_FIELD(symbol), SPP_FIELD(orderId));
SPP_NAMED_RECORD(mcp::tools::Order_Params, "Order_Params",
    SPP_FIELD(symbol), SPP_FIELD(side), SPP_FIELD(type),
    SPP_FIELD(quantity), SPP_FIELD(price));
SPP_NAMED_RECORD(mcp::tools::Chan_Params, "Chan_Params",
    SPP_FIELD(symbol), SPP_FIELD(interval), SPP_FIELD(limit));
SPP_NAMED_RECORD(mcp::tools::Backtest_Params, "Backtest_Params",
    SPP_FIELD(symbol), SPP_FIELD(interval), SPP_FIELD(init_cash), SPP_FIELD(limit));

// Call_Params for tools/call
SPP_NAMED_RECORD(mcp::Call_Params, "Call_Params", SPP_FIELD(name), SPP_FIELD(arguments));

// OKX tool param shapes.
SPP_NAMED_RECORD(mcp::tools::Okx_Symbol_Params, "Okx_Symbol_Params",
    SPP_FIELD(instId));
SPP_NAMED_RECORD(mcp::tools::Okx_Candles_Params, "Okx_Candles_Params",
    SPP_FIELD(instId), SPP_FIELD(bar), SPP_FIELD(limit));
SPP_NAMED_RECORD(mcp::tools::Okx_Chan_Params, "Okx_Chan_Params",
    SPP_FIELD(instId), SPP_FIELD(bar), SPP_FIELD(limit));
SPP_NAMED_RECORD(mcp::tools::Okx_Order_Params, "Okx_Order_Params",
    SPP_FIELD(instId), SPP_FIELD(side), SPP_FIELD(ordType),
    SPP_FIELD(sz), SPP_FIELD(px));

} // namespace spp
