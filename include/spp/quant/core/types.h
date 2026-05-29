#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"

namespace spp::quant {

// =========================================================================
// UNIFIED TYPE DEFINITIONS
//
// These canonical event types replace the previously-separate definitions
// in backtest/event.h and strategy/strategy.h.  Both modules now include
// this single header so that MarketEvent, SignalEvent, OrderEvent, and
// FillEvent share a common layout regardless of whether they flow through
// the backtesting engine or the live strategy framework.
// =========================================================================

// =========================================================================
// Direction / side / status enums
// =========================================================================

/// Directional bias of a signal or trade.
/// Matches the backtest convention: Short = -1, Flat = 0, Long = 1.
enum struct SignalDirection : i8 { Short = -1, Flat = 0, Long = 1 };

/// Side of an order.
enum struct OrderSide : u8 { Buy, Sell };

/// Order type.
enum struct OrderType : u8 { Market, Limit, Stop, StopLimit, IOC, FOK, Iceberg };

/// Order lifecycle status.
enum struct OrderStatus : u8 { New, Partial, Filled, Cancelled, Rejected };

/// High-level event category (used for priority-ordered event queues).
enum struct EventType : u8 { Market = 0, Signal = 1, Order = 2, Fill = 3, Timer = 4 };

// =========================================================================
// MarketEvent — OHLCV bar arriving for one symbol on one date
// =========================================================================
struct MarketEvent {
    Date   date_;
    String_View symbol_;         ///< unified: always symbol_ (strategy used instrument_id_)
    f64    open_   = 0.0;
    f64    high_   = 0.0;
    f64    low_    = 0.0;
    f64    close_  = 0.0;
    f64    volume_ = 0.0;
    u64    trades_count_ = 0;
    f64    vwap_   = 0.0;

    /// Convenience: close price (most common use-case for signals).
    [[nodiscard]] f64 price() const noexcept { return close_; }

    SPP_RECORD(MarketEvent, SPP_FIELD(date_), SPP_FIELD(symbol_),
               SPP_FIELD(open_), SPP_FIELD(high_), SPP_FIELD(low_),
               SPP_FIELD(close_), SPP_FIELD(volume_),
               SPP_FIELD(trades_count_), SPP_FIELD(vwap_));
};

// =========================================================================
// SignalEvent — alpha signal for a symbol
// =========================================================================
struct SignalEvent {
    Date            date_;
    String_View     symbol_;         ///< unified
    SignalDirection direction_    = SignalDirection::Flat;
    f64             strength_     = 0.0;   ///< [-1, 1] confidence / sizing multiplier
    f64             signal_value_ = 0.0;   ///< raw signal value
    f64             confidence_   = 0.0;   ///< [0, 1] additional confidence metric

    SPP_RECORD(SignalEvent, SPP_FIELD(date_), SPP_FIELD(symbol_),
               SPP_FIELD(direction_), SPP_FIELD(strength_),
               SPP_FIELD(signal_value_), SPP_FIELD(confidence_));
};

// =========================================================================
// OrderEvent — order creation and lifecycle state
// =========================================================================
struct OrderEvent {
    u64         order_id_         = 0;
    Date        date_;
    String_View symbol_;         ///< unified
    OrderSide   side_             = OrderSide::Buy;
    OrderType   type_             = OrderType::Market;
    f64         quantity_         = 0.0;
    f64         price_            = 0.0;    ///< 0 for market orders
    f64         stop_price_       = 0.0;    ///< for stop / stop-limit
    f64         visible_quantity_ = 0.0;    ///< for Iceberg orders
    OrderStatus status_           = OrderStatus::New;
    String_View cl_ord_id_;                 ///< client order ID (FIX-style)

    SPP_RECORD(OrderEvent, SPP_FIELD(order_id_), SPP_FIELD(date_),
               SPP_FIELD(symbol_), SPP_FIELD(side_), SPP_FIELD(type_),
               SPP_FIELD(quantity_), SPP_FIELD(price_),
               SPP_FIELD(stop_price_), SPP_FIELD(visible_quantity_),
               SPP_FIELD(status_), SPP_FIELD(cl_ord_id_));
};

// =========================================================================
// FillEvent — execution confirmation
// =========================================================================
struct FillEvent {
    u64       fill_id_    = 0;
    u64       order_id_   = 0;
    Date      date_;
    String_View symbol_;         ///< unified
    OrderSide side_       = OrderSide::Buy;
    f64       quantity_   = 0.0;
    f64       price_      = 0.0;
    f64       commission_ = 0.0;
    f64       slippage_   = 0.0;
    String_View exec_id_;        ///< exchange execution ID

    [[nodiscard]] f64 signed_quantity() const noexcept {
        return (side_ == OrderSide::Buy) ? quantity_ : -quantity_;
    }

    SPP_RECORD(FillEvent, SPP_FIELD(fill_id_), SPP_FIELD(order_id_),
               SPP_FIELD(date_), SPP_FIELD(symbol_), SPP_FIELD(side_),
               SPP_FIELD(quantity_), SPP_FIELD(price_),
               SPP_FIELD(commission_), SPP_FIELD(slippage_),
               SPP_FIELD(exec_id_));
};

// =========================================================================
// TimerEvent — scheduled action trigger (e.g. daily close, rebalance)
// =========================================================================
struct TimerEvent {
    Date        date_;
    String_View timer_name_;
    u64         interval_seconds_ = 0;

    SPP_RECORD(TimerEvent, SPP_FIELD(date_), SPP_FIELD(timer_name_),
               SPP_FIELD(interval_seconds_));
};

// =========================================================================
// AnyEvent — master event variant for event queues
// =========================================================================
using AnyEvent = Variant<MarketEvent, SignalEvent, OrderEvent, FillEvent, TimerEvent>;

} // namespace spp::quant

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_ENUM(::spp::quant::SignalDirection, "SignalDirection", Flat,
               SPP_CASE(Short), SPP_CASE(Flat), SPP_CASE(Long));

SPP_NAMED_ENUM(::spp::quant::OrderSide, "OrderSide", Buy,
               SPP_CASE(Buy), SPP_CASE(Sell));

SPP_NAMED_ENUM(::spp::quant::OrderType, "OrderType", Market,
               SPP_CASE(Market), SPP_CASE(Limit), SPP_CASE(Stop),
               SPP_CASE(StopLimit), SPP_CASE(IOC), SPP_CASE(FOK),
               SPP_CASE(Iceberg));

SPP_NAMED_ENUM(::spp::quant::OrderStatus, "OrderStatus", New,
               SPP_CASE(New), SPP_CASE(Partial), SPP_CASE(Filled),
               SPP_CASE(Cancelled), SPP_CASE(Rejected));

SPP_NAMED_ENUM(::spp::quant::EventType, "EventType", Market,
               SPP_CASE(Market), SPP_CASE(Signal), SPP_CASE(Order),
               SPP_CASE(Fill), SPP_CASE(Timer));

SPP_NAMED_RECORD(::spp::quant::MarketEvent, "MarketEvent",
                 SPP_FIELD(date_), SPP_FIELD(symbol_), SPP_FIELD(open_),
                 SPP_FIELD(high_), SPP_FIELD(low_), SPP_FIELD(close_),
                 SPP_FIELD(volume_), SPP_FIELD(trades_count_), SPP_FIELD(vwap_));

SPP_NAMED_RECORD(::spp::quant::SignalEvent, "SignalEvent",
                 SPP_FIELD(date_), SPP_FIELD(symbol_), SPP_FIELD(direction_),
                 SPP_FIELD(strength_), SPP_FIELD(signal_value_),
                 SPP_FIELD(confidence_));

SPP_NAMED_RECORD(::spp::quant::OrderEvent, "OrderEvent",
                 SPP_FIELD(order_id_), SPP_FIELD(date_), SPP_FIELD(symbol_),
                 SPP_FIELD(side_), SPP_FIELD(type_), SPP_FIELD(quantity_),
                 SPP_FIELD(price_), SPP_FIELD(stop_price_),
                 SPP_FIELD(visible_quantity_), SPP_FIELD(status_),
                 SPP_FIELD(cl_ord_id_));

SPP_NAMED_RECORD(::spp::quant::FillEvent, "FillEvent",
                 SPP_FIELD(fill_id_), SPP_FIELD(order_id_), SPP_FIELD(date_),
                 SPP_FIELD(symbol_), SPP_FIELD(side_), SPP_FIELD(quantity_),
                 SPP_FIELD(price_), SPP_FIELD(commission_), SPP_FIELD(slippage_),
                 SPP_FIELD(exec_id_));

SPP_NAMED_RECORD(::spp::quant::TimerEvent, "TimerEvent",
                 SPP_FIELD(date_), SPP_FIELD(timer_name_),
                 SPP_FIELD(interval_seconds_));
