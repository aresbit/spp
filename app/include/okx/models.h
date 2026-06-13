#pragma once

// OKX V5 wire types.
//
// Two response shapes recur across endpoints:
//
//   1. {"code":"0", "msg":"", "data":[{...}]}   ← REST envelopes
//   2. {"event":"...", "arg":{...}, "data":[...]}  ← WS messages
//
// We model REST envelopes via per-endpoint structs (e.g. `Place_Order_Resp`
// wraps `data: Vec<Place_Order_Item>`) and use SPP reflection to round-trip
// them. WS payloads are inspected via field-by-field extraction since the
// `data` element type varies per channel.
//
// Prices, sizes and timestamps arrive as STRINGS (preserving precision) —
// we keep them as `String<Mdefault>` and let callers convert to `Decimal<>`
// or `i64` only when needed.  This matches Binance's approach for the same
// reason.

#include <spp/core/base.h>
#include <spp/reflection/json.h>

namespace spp::App::Okx {

// ---- Side / OrdType ----
enum class Side : u8 {
    buy,
    sell,
};

enum class Ord_Type : u8 {
    market,
    limit,
    post_only,
    fok,
    ioc,
    optimal_limit_ioc,
};

enum class Td_Mode : u8 {
    cash,         // spot (without margin)
    cross,        // cross margin
    isolated,     // isolated margin
};

// ---- Application-friendly order parameters ----
struct Order_Request {
    String_View instId;      // "BTC-USDT" (spot) / "BTC-USDT-SWAP" (perp)
    Td_Mode tdMode = Td_Mode::cash;
    Side side = Side::buy;
    Ord_Type ordType = Ord_Type::limit;
    String_View sz;          // size as string; "0.001" etc.
    String_View px;          // price as string; empty for market
    String_View clOrdId;     // optional client-supplied id (≤32 chars)
};

// ---- REST: GET /api/v5/public/time ----
struct Server_Time_Item {
    String<Mdefault> ts;     // ms since epoch (string)
};
struct Server_Time_Resp {
    String<Mdefault> code;
    String<Mdefault> msg;
    Vec<Server_Time_Item, Mdefault> data;
};

// ---- REST: POST /api/v5/trade/order ----
struct Place_Order_Item {
    String<Mdefault> clOrdId;
    String<Mdefault> ordId;
    String<Mdefault> tag;
    String<Mdefault> sCode;  // "0" = success; non-zero = per-order error
    String<Mdefault> sMsg;
};
struct Place_Order_Resp {
    String<Mdefault> code;
    String<Mdefault> msg;
    Vec<Place_Order_Item, Mdefault> data;
};

// ---- REST: POST /api/v5/trade/cancel-order ----
struct Cancel_Order_Item {
    String<Mdefault> clOrdId;
    String<Mdefault> ordId;
    String<Mdefault> sCode;
    String<Mdefault> sMsg;
};
struct Cancel_Order_Resp {
    String<Mdefault> code;
    String<Mdefault> msg;
    Vec<Cancel_Order_Item, Mdefault> data;
};

// ---- REST: GET /api/v5/account/balance ----
struct Balance_Detail {
    String<Mdefault> ccy;        // "USDT" / "BTC"
    String<Mdefault> eq;         // total equity
    String<Mdefault> availBal;   // available balance
    String<Mdefault> frozenBal;  // frozen balance
};
struct Balance_Group {
    String<Mdefault> uTime;
    String<Mdefault> totalEq;
    String<Mdefault> isoEq;
    Vec<Balance_Detail, Mdefault> details;
};
struct Balance_Resp {
    String<Mdefault> code;
    String<Mdefault> msg;
    Vec<Balance_Group, Mdefault> data;
};

// ---- REST: GET /api/v5/market/ticker ----
struct Ticker_Item {
    String<Mdefault> instType;
    String<Mdefault> instId;
    String<Mdefault> last;
    String<Mdefault> lastSz;
    String<Mdefault> askPx;
    String<Mdefault> askSz;
    String<Mdefault> bidPx;
    String<Mdefault> bidSz;
    String<Mdefault> ts;
};
struct Ticker_Resp {
    String<Mdefault> code;
    String<Mdefault> msg;
    Vec<Ticker_Item, Mdefault> data;
};

// ---- REST: GET /api/v5/public/instruments ----
struct Instrument_Item {
    String<Mdefault> instType;
    String<Mdefault> instId;
    String<Mdefault> tickSz;
    String<Mdefault> lotSz;
    String<Mdefault> minSz;
};
struct Instruments_Resp {
    String<Mdefault> code;
    String<Mdefault> msg;
    Vec<Instrument_Item, Mdefault> data;
};

// ---- REST: GET /api/v5/market/candles ----
// The wire payload is array-of-arrays, parsed by hand (mirrors Binance's
// Kline).  Fields per row:
//   [ts, o, h, l, c, vol, volCcy, volCcyQuote, confirm]
struct Candlestick {
    i64 open_time = 0;
    String<Mdefault> open;
    String<Mdefault> high;
    String<Mdefault> low;
    String<Mdefault> close;
    String<Mdefault> vol;          // base-asset volume
    String<Mdefault> volCcy;       // quote-asset volume
    String<Mdefault> volCcyQuote;  // quote-asset volume in USD-equivalent
    i32 confirm = 0;               // 1 = closed bar
};

// ---- Wire-name helpers (enum → string) ----
[[nodiscard]] inline String_View wire_name(Side s) noexcept {
    return s == Side::buy ? "buy"_v : "sell"_v;
}
[[nodiscard]] inline String_View wire_name(Ord_Type t) noexcept {
    switch(t) {
    case Ord_Type::market:            return "market"_v;
    case Ord_Type::limit:             return "limit"_v;
    case Ord_Type::post_only:         return "post_only"_v;
    case Ord_Type::fok:               return "fok"_v;
    case Ord_Type::ioc:               return "ioc"_v;
    case Ord_Type::optimal_limit_ioc: return "optimal_limit_ioc"_v;
    }
    return "limit"_v;
}
[[nodiscard]] inline String_View wire_name(Td_Mode m) noexcept {
    switch(m) {
    case Td_Mode::cash:     return "cash"_v;
    case Td_Mode::cross:    return "cross"_v;
    case Td_Mode::isolated: return "isolated"_v;
    }
    return "cash"_v;
}

} // namespace spp::App::Okx

namespace spp {

SPP_NAMED_RECORD(App::Okx::Server_Time_Item, "Okx_Server_Time_Item",
                 SPP_FIELD(ts));
SPP_NAMED_RECORD(App::Okx::Server_Time_Resp, "Okx_Server_Time_Resp",
                 SPP_FIELD(code), SPP_FIELD(msg), SPP_FIELD(data));

SPP_NAMED_RECORD(App::Okx::Place_Order_Item, "Okx_Place_Order_Item",
                 SPP_FIELD(clOrdId), SPP_FIELD(ordId),
                 SPP_FIELD(tag), SPP_FIELD(sCode), SPP_FIELD(sMsg));
SPP_NAMED_RECORD(App::Okx::Place_Order_Resp, "Okx_Place_Order_Resp",
                 SPP_FIELD(code), SPP_FIELD(msg), SPP_FIELD(data));

SPP_NAMED_RECORD(App::Okx::Cancel_Order_Item, "Okx_Cancel_Order_Item",
                 SPP_FIELD(clOrdId), SPP_FIELD(ordId),
                 SPP_FIELD(sCode), SPP_FIELD(sMsg));
SPP_NAMED_RECORD(App::Okx::Cancel_Order_Resp, "Okx_Cancel_Order_Resp",
                 SPP_FIELD(code), SPP_FIELD(msg), SPP_FIELD(data));

SPP_NAMED_RECORD(App::Okx::Balance_Detail, "Okx_Balance_Detail",
                 SPP_FIELD(ccy), SPP_FIELD(eq),
                 SPP_FIELD(availBal), SPP_FIELD(frozenBal));
SPP_NAMED_RECORD(App::Okx::Balance_Group, "Okx_Balance_Group",
                 SPP_FIELD(uTime), SPP_FIELD(totalEq),
                 SPP_FIELD(isoEq), SPP_FIELD(details));
SPP_NAMED_RECORD(App::Okx::Balance_Resp, "Okx_Balance_Resp",
                 SPP_FIELD(code), SPP_FIELD(msg), SPP_FIELD(data));

SPP_NAMED_RECORD(App::Okx::Instrument_Item, "Okx_Instrument_Item",
                 SPP_FIELD(instType), SPP_FIELD(instId),
                 SPP_FIELD(tickSz), SPP_FIELD(lotSz), SPP_FIELD(minSz));
SPP_NAMED_RECORD(App::Okx::Instruments_Resp, "Okx_Instruments_Resp",
                 SPP_FIELD(code), SPP_FIELD(msg), SPP_FIELD(data));

SPP_NAMED_RECORD(App::Okx::Ticker_Item, "Okx_Ticker_Item",
                 SPP_FIELD(instType), SPP_FIELD(instId),
                 SPP_FIELD(last), SPP_FIELD(lastSz),
                 SPP_FIELD(askPx), SPP_FIELD(askSz),
                 SPP_FIELD(bidPx), SPP_FIELD(bidSz),
                 SPP_FIELD(ts));
SPP_NAMED_RECORD(App::Okx::Ticker_Resp, "Okx_Ticker_Resp",
                 SPP_FIELD(code), SPP_FIELD(msg), SPP_FIELD(data));

SPP_NAMED_ENUM(App::Okx::Side, "Okx_Side",
               buy, SPP_CASE(buy), SPP_CASE(sell));
SPP_NAMED_ENUM(App::Okx::Ord_Type, "Okx_Ord_Type",
               limit, SPP_CASE(market), SPP_CASE(limit),
               SPP_CASE(post_only), SPP_CASE(fok), SPP_CASE(ioc),
               SPP_CASE(optimal_limit_ioc));
SPP_NAMED_ENUM(App::Okx::Td_Mode, "Okx_Td_Mode",
               cash, SPP_CASE(cash), SPP_CASE(cross), SPP_CASE(isolated));

} // namespace spp
