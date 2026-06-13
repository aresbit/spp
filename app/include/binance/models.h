#pragma once

#include <spp/core/base.h>
#include <spp/reflection/json.h>

// Typed Binance Spot wire types. Field names mirror Binance's JSON exactly
// (camelCase) so SPP_RECORD's `#FIELD`-as-key default round-trips cleanly via
// spp::Json::stringify / Json::parse_result.
//
// Prices and quantities arrive as strings to preserve precision — we keep
// them as String<Mdefault> here and let the caller convert to Decimal<>
// (deterministic.h) when arithmetic is needed. Identifiers and timestamps
// arrive as JSON numbers and stay as i64.

namespace spp::App::Binance {

struct Server_Time {
    i64 serverTime = 0;
};

struct Ticker_Price {
    String<Mdefault> symbol;
    String<Mdefault> price;
};

struct Order_Book_Level {
    String<Mdefault> price;
    String<Mdefault> qty;
};

struct Account_Balance {
    String<Mdefault> asset;
    String<Mdefault> free;
    String<Mdefault> locked;
};

struct Account_Info {
    i64 makerCommission = 0;
    i64 takerCommission = 0;
    i64 updateTime = 0;
    bool canTrade = false;
    bool canWithdraw = false;
    bool canDeposit = false;
    Vec<Account_Balance, Mdefault> balances;
};

// Subset of Binance's Spot order side / type / time-in-force / status enums.
// We use plain enum struct to keep wire conversion explicit on the call site;
// JSON parsing for the typed Order types treats them as strings.
enum class Side : u8 {
    BUY,
    SELL,
};

enum class Order_Type : u8 {
    LIMIT,
    MARKET,
    STOP_LOSS,
    STOP_LOSS_LIMIT,
    TAKE_PROFIT,
    TAKE_PROFIT_LIMIT,
    LIMIT_MAKER,
};

enum class Time_In_Force : u8 {
    GTC,
    IOC,
    FOK,
};

enum class Order_Status : u8 {
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    PENDING_CANCEL,
    REJECTED,
    EXPIRED,
    EXPIRED_IN_MATCH,
};

// Application-friendly parameters for POST /api/v3/order. The signer turns
// this into a query string; we keep prices/quantities as String to avoid
// f64 rounding before the wire.
struct Order_Request {
    String_View symbol;
    Side side = Side::BUY;
    Order_Type type = Order_Type::LIMIT;
    Opt<Time_In_Force> time_in_force;
    String_View quantity;
    String_View price;          // for LIMIT-family
    String_View stop_price;     // for STOP / TAKE_PROFIT family
    String_View client_order_id; // optional newClientOrderId override
};

// POST /api/v3/order response (ACK variant — defaults to ACK; FULL adds fills).
struct Order_Response {
    String<Mdefault> symbol;
    i64 orderId = 0;
    i64 orderListId = -1;
    String<Mdefault> clientOrderId;
    i64 transactTime = 0;
    String<Mdefault> price;
    String<Mdefault> origQty;
    String<Mdefault> executedQty;
    String<Mdefault> cummulativeQuoteQty;
    String<Mdefault> status;
    String<Mdefault> timeInForce;
    String<Mdefault> type;
    String<Mdefault> side;
};

// GET /api/v3/order response. Same shape as the canonical query result.
struct Order_State {
    String<Mdefault> symbol;
    i64 orderId = 0;
    i64 orderListId = -1;
    String<Mdefault> clientOrderId;
    String<Mdefault> price;
    String<Mdefault> origQty;
    String<Mdefault> executedQty;
    String<Mdefault> cummulativeQuoteQty;
    String<Mdefault> status;
    String<Mdefault> timeInForce;
    String<Mdefault> type;
    String<Mdefault> side;
    i64 time = 0;
    i64 updateTime = 0;
    bool isWorking = false;
};

// Kline/candlestick data. Binance returns this as an array-of-arrays, not an
// object, so we parse it manually rather than via SPP_RECORD. This struct is
// the typed representation after conversion.
struct Kline {
    i64 open_time = 0;
    String<Mdefault> open;
    String<Mdefault> high;
    String<Mdefault> low;
    String<Mdefault> close;
    String<Mdefault> volume;
    i64 close_time = 0;
    String<Mdefault> quote_volume;
    i32 trades = 0;
    String<Mdefault> taker_buy_base;
    String<Mdefault> taker_buy_quote;
    String<Mdefault> unused;
};

// GET /api/v3/ticker/bookTicker response.
struct Book_Ticker {
    String<Mdefault> symbol;
    String<Mdefault> bidPrice;
    String<Mdefault> bidQty;
    String<Mdefault> askPrice;
    String<Mdefault> askQty;
};

// GET /api/v3/exchangeInfo response (subset: symbol filters used for
// lot-size / price-tick validation).
struct Symbol_Filter {
    String<Mdefault> filterType;
    String<Mdefault> minPrice;
    String<Mdefault> maxPrice;
    String<Mdefault> tickSize;
    String<Mdefault> minQty;
    String<Mdefault> maxQty;
    String<Mdefault> stepSize;
    String<Mdefault> minNotional;
};

struct Exchange_Symbol {
    String<Mdefault> symbol;
    String<Mdefault> status;
    String<Mdefault> baseAsset;
    String<Mdefault> quoteAsset;
    Vec<Symbol_Filter, Mdefault> filters;
};

struct Exchange_Info {
    Vec<Exchange_Symbol, Mdefault> symbols;
};

} // namespace spp::App::Binance

namespace spp {

SPP_NAMED_RECORD(App::Binance::Server_Time, "Server_Time", SPP_FIELD(serverTime));
SPP_NAMED_RECORD(App::Binance::Ticker_Price, "Ticker_Price", SPP_FIELD(symbol),
                 SPP_FIELD(price));
SPP_NAMED_RECORD(App::Binance::Order_Book_Level, "Order_Book_Level", SPP_FIELD(price),
                 SPP_FIELD(qty));
SPP_NAMED_RECORD(App::Binance::Account_Balance, "Account_Balance", SPP_FIELD(asset),
                 SPP_FIELD(free), SPP_FIELD(locked));
SPP_NAMED_RECORD(App::Binance::Account_Info, "Account_Info", SPP_FIELD(makerCommission),
                 SPP_FIELD(takerCommission), SPP_FIELD(updateTime), SPP_FIELD(canTrade),
                 SPP_FIELD(canWithdraw), SPP_FIELD(canDeposit), SPP_FIELD(balances));
SPP_NAMED_RECORD(App::Binance::Order_Response, "Order_Response", SPP_FIELD(symbol),
                 SPP_FIELD(orderId), SPP_FIELD(orderListId), SPP_FIELD(clientOrderId),
                 SPP_FIELD(transactTime), SPP_FIELD(price), SPP_FIELD(origQty),
                 SPP_FIELD(executedQty), SPP_FIELD(cummulativeQuoteQty), SPP_FIELD(status),
                 SPP_FIELD(timeInForce), SPP_FIELD(type), SPP_FIELD(side));
SPP_NAMED_RECORD(App::Binance::Order_State, "Order_State", SPP_FIELD(symbol),
                 SPP_FIELD(orderId), SPP_FIELD(orderListId), SPP_FIELD(clientOrderId),
                 SPP_FIELD(price), SPP_FIELD(origQty), SPP_FIELD(executedQty),
                 SPP_FIELD(cummulativeQuoteQty), SPP_FIELD(status), SPP_FIELD(timeInForce),
                 SPP_FIELD(type), SPP_FIELD(side), SPP_FIELD(time), SPP_FIELD(updateTime),
                 SPP_FIELD(isWorking));

SPP_NAMED_RECORD(App::Binance::Book_Ticker, "Book_Ticker", SPP_FIELD(symbol),
                 SPP_FIELD(bidPrice), SPP_FIELD(bidQty), SPP_FIELD(askPrice), SPP_FIELD(askQty));
SPP_NAMED_RECORD(App::Binance::Symbol_Filter, "Symbol_Filter", SPP_FIELD(filterType),
                 SPP_FIELD(minPrice), SPP_FIELD(maxPrice), SPP_FIELD(tickSize),
                 SPP_FIELD(minQty), SPP_FIELD(maxQty), SPP_FIELD(stepSize), SPP_FIELD(minNotional));
SPP_NAMED_RECORD(App::Binance::Exchange_Symbol, "Exchange_Symbol", SPP_FIELD(symbol),
                 SPP_FIELD(status), SPP_FIELD(baseAsset), SPP_FIELD(quoteAsset), SPP_FIELD(filters));
SPP_NAMED_RECORD(App::Binance::Exchange_Info, "Exchange_Info", SPP_FIELD(symbols));

SPP_NAMED_ENUM(App::Binance::Side, "Side", BUY, SPP_CASE(BUY), SPP_CASE(SELL));
SPP_NAMED_ENUM(App::Binance::Order_Type, "Order_Type", LIMIT, SPP_CASE(LIMIT), SPP_CASE(MARKET),
               SPP_CASE(STOP_LOSS), SPP_CASE(STOP_LOSS_LIMIT), SPP_CASE(TAKE_PROFIT),
               SPP_CASE(TAKE_PROFIT_LIMIT), SPP_CASE(LIMIT_MAKER));
SPP_NAMED_ENUM(App::Binance::Time_In_Force, "Time_In_Force", GTC, SPP_CASE(GTC), SPP_CASE(IOC),
               SPP_CASE(FOK));
SPP_NAMED_ENUM(App::Binance::Order_Status, "Order_Status", NEW, SPP_CASE(NEW),
               SPP_CASE(PARTIALLY_FILLED), SPP_CASE(FILLED), SPP_CASE(CANCELED),
               SPP_CASE(PENDING_CANCEL), SPP_CASE(REJECTED), SPP_CASE(EXPIRED),
               SPP_CASE(EXPIRED_IN_MATCH));

namespace App::Binance {

// Wire-name helpers (Binance expects ALL-CAPS enum names verbatim in query
// strings — same casing as our C++ enumerator names).
[[nodiscard]] inline String_View wire_name(Side s) noexcept {
    return s == Side::BUY ? "BUY"_v : "SELL"_v;
}
[[nodiscard]] inline String_View wire_name(Order_Type t) noexcept {
    switch(t) {
    case Order_Type::LIMIT: return "LIMIT"_v;
    case Order_Type::MARKET: return "MARKET"_v;
    case Order_Type::STOP_LOSS: return "STOP_LOSS"_v;
    case Order_Type::STOP_LOSS_LIMIT: return "STOP_LOSS_LIMIT"_v;
    case Order_Type::TAKE_PROFIT: return "TAKE_PROFIT"_v;
    case Order_Type::TAKE_PROFIT_LIMIT: return "TAKE_PROFIT_LIMIT"_v;
    case Order_Type::LIMIT_MAKER: return "LIMIT_MAKER"_v;
    }
    return "LIMIT"_v;
}
[[nodiscard]] inline String_View wire_name(Time_In_Force t) noexcept {
    switch(t) {
    case Time_In_Force::GTC: return "GTC"_v;
    case Time_In_Force::IOC: return "IOC"_v;
    case Time_In_Force::FOK: return "FOK"_v;
    }
    return "GTC"_v;
}

} // namespace App::Binance

} // namespace spp
