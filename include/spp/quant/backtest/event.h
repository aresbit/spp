#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"

namespace spp::quant::backtest {

// =========================================================================
// Event priority: lower value = higher priority in event queue
// Market (0) -> Signal (1) -> Order (2) -> Fill (3) -> Timer (4)
// =========================================================================
enum struct EventType : u8 { Market = 0, Signal = 1, Order = 2, Fill = 3, Timer = 4 };

// =========================================================================
// MarketEvent — OHLCV bar for one symbol on one date
// =========================================================================
struct MarketEvent {
    Date   date_;
    f64    open_   = 0.0;
    f64    high_   = 0.0;
    f64    low_    = 0.0;
    f64    close_  = 0.0;
    f64    volume_ = 0.0;

    SPP_RECORD(MarketEvent, SPP_FIELD(date_), SPP_FIELD(open_), SPP_FIELD(high_),
               SPP_FIELD(low_), SPP_FIELD(close_), SPP_FIELD(volume_));
};

// =========================================================================
// SignalDirection — directional bias
// =========================================================================
enum struct SignalDirection : i8 { Short = -1, Flat = 0, Long = 1 };

// =========================================================================
// SignalEvent — alpha signal for a symbol
// =========================================================================
struct SignalEvent {
    Date            date_;
    SignalDirection direction_    = SignalDirection::Flat;
    f64             strength_     = 0.0;   // [-1, 1] confidence / sizing multiplier
    f64             signal_value_ = 0.0;   // raw signal value

    SPP_RECORD(SignalEvent, SPP_FIELD(date_), SPP_FIELD(direction_),
               SPP_FIELD(strength_), SPP_FIELD(signal_value_));
};

// =========================================================================
// Order types and sides
// =========================================================================
enum struct OrderSide   : u8 { Buy, Sell };
enum struct OrderType   : u8 { Market, Limit, Stop, StopLimit, IOC, FOK, Iceberg };
enum struct OrderStatus : u8 { New, Partial, Filled, Cancelled, Rejected };

// =========================================================================
// OrderEvent — order creation and state
// =========================================================================
struct OrderEvent {
    u64         order_id_         = 0;
    Date        date_;
    OrderSide   side_             = OrderSide::Buy;
    OrderType   type_             = OrderType::Market;
    f64         quantity_         = 0.0;
    f64         price_            = 0.0;    // 0 for market orders
    f64         stop_price_       = 0.0;    // for stop / stop-limit
    f64         visible_quantity_ = 0.0;    // for iceberg orders
    OrderStatus status_           = OrderStatus::New;

    SPP_RECORD(OrderEvent, SPP_FIELD(order_id_), SPP_FIELD(date_), SPP_FIELD(side_),
               SPP_FIELD(type_), SPP_FIELD(quantity_), SPP_FIELD(price_),
               SPP_FIELD(stop_price_), SPP_FIELD(visible_quantity_), SPP_FIELD(status_));
};

// =========================================================================
// FillEvent — execution confirmation
// =========================================================================
struct FillEvent {
    u64       fill_id_    = 0;
    u64       order_id_   = 0;
    Date      date_;
    OrderSide side_       = OrderSide::Buy;
    f64       quantity_   = 0.0;
    f64       price_      = 0.0;
    f64       commission_ = 0.0;
    f64       slippage_   = 0.0;

    SPP_RECORD(FillEvent, SPP_FIELD(fill_id_), SPP_FIELD(order_id_), SPP_FIELD(date_),
               SPP_FIELD(side_), SPP_FIELD(quantity_), SPP_FIELD(price_),
               SPP_FIELD(commission_), SPP_FIELD(slippage_));
};

// =========================================================================
// AnyEvent — master event variant (must be Ordered for Heap)
// =========================================================================
using AnyEvent = Variant<MarketEvent, SignalEvent, OrderEvent, FillEvent>;

// =========================================================================
// EventQueueEntry — wrapper for Heap-based event priority queue.
//
// Min-heap: earlier dates (smaller serial_) bubble to the top.
// On same date, events are ordered by type priority:
//   Market(0) < Signal(1) < Order(2) < Fill(3) < Timer(4)
// =========================================================================
struct EventQueueEntry {
    Date     date_;
    AnyEvent event_;

    EventQueueEntry() = default;
    EventQueueEntry(Date d, AnyEvent e) noexcept
        : date_(d), event_(spp::move(e)) {}

    EventQueueEntry(EventQueueEntry&&) noexcept = default;
    EventQueueEntry& operator=(EventQueueEntry&&) noexcept = default;

    // Copy is explicitly allowed for clone operations
    // Variant is not copy-constructable, so we delete copy
    EventQueueEntry(const EventQueueEntry&) = delete;
    EventQueueEntry& operator=(const EventQueueEntry&) = delete;

    [[nodiscard]] bool operator<(const EventQueueEntry& other) const noexcept {
        if (date_ != other.date_) return date_ < other.date_;
        return type_priority() < other.type_priority();
    }

    [[nodiscard]] u8 type_priority() const noexcept {
        return event_.match(Overload{
            [](const MarketEvent&)  { return static_cast<u8>(0); },
            [](const SignalEvent&)  { return static_cast<u8>(1); },
            [](const OrderEvent&)   { return static_cast<u8>(2); },
            [](const FillEvent&)    { return static_cast<u8>(3); },
        });
    }

    static EventQueueEntry clone(const EventQueueEntry& src) noexcept {
        EventQueueEntry result;
        result.date_  = src.date_;
        result.event_ = src.event_.clone();
        return result;
    }
};

// =========================================================================
// event_priority — convert an AnyEvent to its event type enum value
// =========================================================================
inline EventType event_type(const AnyEvent& e) noexcept {
    return e.match(Overload{
        [](const MarketEvent&)  { return EventType::Market;  },
        [](const SignalEvent&)  { return EventType::Signal;  },
        [](const OrderEvent&)   { return EventType::Order;   },
        [](const FillEvent&)    { return EventType::Fill;    },
    });
}

// =========================================================================
// event_priority — lower value = higher priority (for display / util)
// =========================================================================
inline i32 event_priority(const AnyEvent& e) noexcept {
    return e.match(Overload{
        [](const MarketEvent&)  { return 0; },
        [](const SignalEvent&)  { return 1; },
        [](const OrderEvent&)   { return 2; },
        [](const FillEvent&)    { return 3; },
    });
}

// =========================================================================
// Helper to extract the event date regardless of event type
// =========================================================================
inline Date event_date(const AnyEvent& e) noexcept {
    return e.match(Overload{
        [](const MarketEvent&  ev) { return ev.date_; },
        [](const SignalEvent&  ev) { return ev.date_; },
        [](const OrderEvent&   ev) { return ev.date_; },
        [](const FillEvent&    ev) { return ev.date_; },
    });
}

} // namespace spp::quant::backtest

SPP_NAMED_ENUM(::spp::quant::backtest::EventType, "EventType", Market,
               SPP_CASE(Market), SPP_CASE(Signal), SPP_CASE(Order), SPP_CASE(Fill),
               SPP_CASE(Timer));

SPP_NAMED_RECORD(::spp::quant::backtest::MarketEvent, "MarketEvent",
                 SPP_FIELD(date_), SPP_FIELD(open_), SPP_FIELD(high_),
                 SPP_FIELD(low_), SPP_FIELD(close_), SPP_FIELD(volume_));

SPP_NAMED_ENUM(::spp::quant::backtest::SignalDirection, "SignalDirection", Flat,
               SPP_CASE(Short), SPP_CASE(Flat), SPP_CASE(Long));

SPP_NAMED_RECORD(::spp::quant::backtest::SignalEvent, "SignalEvent",
                 SPP_FIELD(date_), SPP_FIELD(direction_), SPP_FIELD(strength_),
                 SPP_FIELD(signal_value_));

SPP_NAMED_ENUM(::spp::quant::backtest::OrderSide, "OrderSide", Buy,
               SPP_CASE(Buy), SPP_CASE(Sell));

SPP_NAMED_ENUM(::spp::quant::backtest::OrderType, "OrderType", Market,
               SPP_CASE(Market), SPP_CASE(Limit), SPP_CASE(Stop), SPP_CASE(StopLimit),
               SPP_CASE(IOC), SPP_CASE(FOK), SPP_CASE(Iceberg));

SPP_NAMED_ENUM(::spp::quant::backtest::OrderStatus, "OrderStatus", New,
               SPP_CASE(New), SPP_CASE(Partial), SPP_CASE(Filled),
               SPP_CASE(Cancelled), SPP_CASE(Rejected));

SPP_NAMED_RECORD(::spp::quant::backtest::OrderEvent, "OrderEvent",
                 SPP_FIELD(order_id_), SPP_FIELD(date_), SPP_FIELD(side_),
                 SPP_FIELD(type_), SPP_FIELD(quantity_), SPP_FIELD(price_),
                 SPP_FIELD(stop_price_), SPP_FIELD(visible_quantity_), SPP_FIELD(status_));

SPP_NAMED_RECORD(::spp::quant::backtest::FillEvent, "FillEvent",
                 SPP_FIELD(fill_id_), SPP_FIELD(order_id_), SPP_FIELD(date_),
                 SPP_FIELD(side_), SPP_FIELD(quantity_), SPP_FIELD(price_),
                 SPP_FIELD(commission_), SPP_FIELD(slippage_));

SPP_TEMPLATE_RECORD(::spp::quant::backtest::EventQueueEntry, SPP_PACK(),
                     SPP_FIELD(date_));  // event_ field is Variant — opaque to reflection
