#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/core/types.h"  // unified event types in spp::quant

namespace spp::quant::backtest {

// =========================================================================
// Re-export unified types for backward compatibility.
//
// Event types are now defined ONCE in spp::quant (core/types.h).
// These using-declarations keep spp::quant::backtest::MarketEvent (etc.)
// working for existing code.
// =========================================================================
using spp::quant::EventType;
using spp::quant::MarketEvent;
using spp::quant::SignalDirection;
using spp::quant::SignalEvent;
using spp::quant::OrderSide;
using spp::quant::OrderType;
using spp::quant::OrderStatus;
using spp::quant::OrderEvent;
using spp::quant::FillEvent;
using spp::quant::TimerEvent;
using spp::quant::AnyEvent;

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

    // Copy is explicitly allowed for clone operations.
    // Variant is not copy-constructable, so we delete copy.
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
            [](const TimerEvent&)   { return static_cast<u8>(4); },
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
// event_type — convert an AnyEvent to its event type enum value
// =========================================================================
inline EventType event_type(const AnyEvent& e) noexcept {
    return e.match(Overload{
        [](const MarketEvent&)  { return EventType::Market;  },
        [](const SignalEvent&)  { return EventType::Signal;  },
        [](const OrderEvent&)   { return EventType::Order;   },
        [](const FillEvent&)    { return EventType::Fill;    },
        [](const TimerEvent&)   { return EventType::Timer;   },
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
        [](const TimerEvent&)   { return 4; },
    });
}

// =========================================================================
// event_date — extract the event date regardless of event type
// =========================================================================
inline Date event_date(const AnyEvent& e) noexcept {
    return e.match(Overload{
        [](const MarketEvent&  ev) { return ev.date_; },
        [](const SignalEvent&  ev) { return ev.date_; },
        [](const OrderEvent&   ev) { return ev.date_; },
        [](const FillEvent&    ev) { return ev.date_; },
        [](const TimerEvent&   ev) { return ev.date_; },
    });
}

} // namespace spp::quant::backtest

// Only EventQueueEntry needs a reflection record here; all other types
// are now reflected in core/types.h
SPP_TEMPLATE_RECORD(::spp::quant::backtest::EventQueueEntry, SPP_PACK(),
                     SPP_FIELD(date_));  // event_ field is Variant — opaque to reflection
