#pragma once

#include <spp/core/base.h>
#include <spp/containers/string0.h>
#include <spp/containers/vec.h>
#include <spp/core/deterministic.h>

namespace spp::quant::strategy {

// ============================================================
// Enumerations
// ============================================================

// ---- Order Direction ----
enum class Order_Direction : u8 {
    buy_open,
    sell_open,
    buy_close,
    sell_close,
    buy,
    sell
};

// ---- Order Offset ----
enum class Order_Offset : u8 {
    open_,
    close_,
    none_
};

// ---- Running Mode ----
enum class Running_Mode : u8 {
    backtest,
    sim,
    live
};

// ---- Market Type ----
enum class Market_Type : u8 {
    stock_cn,
    future_cn,
    crypto
};

// ---- Frequency ----
enum class Frequency : u8 {
    tick,
    min1,
    min5,
    min15,
    min30,
    min60,
    day,
    week,
    month
};

// ============================================================
// Record Types
// ============================================================

// ---- Order ----
struct Order {
    String<Mdefault> order_id;
    String<Mdefault> code;
    Order_Direction direction = Order_Direction::buy;
    Order_Offset offset = Order_Offset::open_;
    Decimal<8> price;
    f64 volume = 0.0;
    Deterministic_Time time;
    u8 status = 0;  // 0=pending, 1=filled, 2=cancelled, 3=rejected
};

// ---- Position ----
struct Position {
    String<Mdefault> code;
    f64 volume_long = 0.0;
    f64 volume_short = 0.0;
    Decimal<8> open_price_long;
    Decimal<8> open_price_short;
    Decimal<8> last_price;
    f64 float_profit = 0.0;  // unrealized PnL
    f64 margin_used = 0.0;

    [[nodiscard]] f64 net_volume() const noexcept {
        return volume_long - volume_short;
    }

    [[nodiscard]] f64 gross_volume() const noexcept {
        return volume_long + volume_short;
    }
};

// ---- Trade ----
struct Trade {
    String<Mdefault> trade_id;
    String<Mdefault> order_id;
    String<Mdefault> code;
    Order_Direction direction = Order_Direction::buy;
    Order_Offset offset = Order_Offset::open_;
    Decimal<8> price;
    f64 volume = 0.0;
    f64 fee = 0.0;
    Deterministic_Time time;
};

// ---- Account Snapshot ----
struct Account_Snapshot {
    Deterministic_Time time;
    f64 balance = 0.0;
    f64 available = 0.0;
    f64 frozen = 0.0;
    f64 equity = 0.0;      // balance + float_profit
    f64 total_pnl = 0.0;
};

// ---- Strategy Signal ----
struct Strategy_Signal {
    Deterministic_Time time;
    String<Mdefault> name;
    f64 value = 0.0;
    String<Mdefault> format;  // "line", "bar", "scatter"
};

} // namespace spp::quant::strategy

// ============================================================
// Reflection specializations (must be in namespace spp,
// the enclosing namespace of spp::Reflect::Refl)
// ============================================================

namespace spp {

SPP_NAMED_ENUM(::spp::quant::strategy::Order_Direction, "Order_Direction",
    buy_open,
    SPP_CASE(buy_open), SPP_CASE(sell_open), SPP_CASE(buy_close),
    SPP_CASE(sell_close), SPP_CASE(buy), SPP_CASE(sell));

SPP_NAMED_ENUM(::spp::quant::strategy::Order_Offset, "Order_Offset",
    open_,
    SPP_CASE(open_), SPP_CASE(close_), SPP_CASE(none_));

SPP_NAMED_ENUM(::spp::quant::strategy::Running_Mode, "Running_Mode",
    backtest,
    SPP_CASE(backtest), SPP_CASE(sim), SPP_CASE(live));

SPP_NAMED_ENUM(::spp::quant::strategy::Market_Type, "Market_Type",
    stock_cn,
    SPP_CASE(stock_cn), SPP_CASE(future_cn), SPP_CASE(crypto));

SPP_NAMED_ENUM(::spp::quant::strategy::Frequency, "Frequency",
    day,
    SPP_CASE(tick), SPP_CASE(min1), SPP_CASE(min5), SPP_CASE(min15),
    SPP_CASE(min30), SPP_CASE(min60), SPP_CASE(day), SPP_CASE(week), SPP_CASE(month));

SPP_NAMED_RECORD(::spp::quant::strategy::Order, "Order",
    SPP_FIELD(order_id),
    SPP_FIELD(code),
    SPP_FIELD(direction),
    SPP_FIELD(offset),
    SPP_FIELD(price),
    SPP_FIELD(volume),
    SPP_FIELD(time),
    SPP_FIELD(status));

SPP_NAMED_RECORD(::spp::quant::strategy::Position, "Position",
    SPP_FIELD(code),
    SPP_FIELD(volume_long),
    SPP_FIELD(volume_short),
    SPP_FIELD(open_price_long),
    SPP_FIELD(open_price_short),
    SPP_FIELD(last_price),
    SPP_FIELD(float_profit),
    SPP_FIELD(margin_used));

SPP_NAMED_RECORD(::spp::quant::strategy::Trade, "Trade",
    SPP_FIELD(trade_id),
    SPP_FIELD(order_id),
    SPP_FIELD(code),
    SPP_FIELD(direction),
    SPP_FIELD(offset),
    SPP_FIELD(price),
    SPP_FIELD(volume),
    SPP_FIELD(fee),
    SPP_FIELD(time));

SPP_NAMED_RECORD(::spp::quant::strategy::Account_Snapshot, "Account_Snapshot",
    SPP_FIELD(time),
    SPP_FIELD(balance),
    SPP_FIELD(available),
    SPP_FIELD(frozen),
    SPP_FIELD(equity),
    SPP_FIELD(total_pnl));

SPP_NAMED_RECORD(::spp::quant::strategy::Strategy_Signal, "Strategy_Signal",
    SPP_FIELD(time),
    SPP_FIELD(name),
    SPP_FIELD(value),
    SPP_FIELD(format));

// Hash specializations so Map<enum, ...> keys work. Hash<Int> excludes enums
// (they aren't Int), so each strategy enum needs its own hook.
namespace Hash {

template<>
struct Hash<::spp::quant::strategy::Order_Direction> {
    [[nodiscard]] constexpr static u64
    hash(::spp::quant::strategy::Order_Direction key) noexcept {
        return squirrel5(static_cast<u64>(static_cast<u8>(key)));
    }
};

template<>
struct Hash<::spp::quant::strategy::Order_Offset> {
    [[nodiscard]] constexpr static u64
    hash(::spp::quant::strategy::Order_Offset key) noexcept {
        return squirrel5(static_cast<u64>(static_cast<u8>(key)));
    }
};

template<>
struct Hash<::spp::quant::strategy::Running_Mode> {
    [[nodiscard]] constexpr static u64
    hash(::spp::quant::strategy::Running_Mode key) noexcept {
        return squirrel5(static_cast<u64>(static_cast<u8>(key)));
    }
};

template<>
struct Hash<::spp::quant::strategy::Market_Type> {
    [[nodiscard]] constexpr static u64
    hash(::spp::quant::strategy::Market_Type key) noexcept {
        return squirrel5(static_cast<u64>(static_cast<u8>(key)));
    }
};

template<>
struct Hash<::spp::quant::strategy::Frequency> {
    [[nodiscard]] constexpr static u64
    hash(::spp::quant::strategy::Frequency key) noexcept {
        return squirrel5(static_cast<u64>(static_cast<u8>(key)));
    }
};

} // namespace Hash

} // namespace spp
