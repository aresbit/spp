#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/backtest/event.h"      // OrderSide, OrderType, OrderStatus
#include "spp/quant/base/date.h"

namespace spp::quant::execution {

// =========================================================================
// Order — full order state machine for live/backtest execution
//
// Tracks lifecycle from creation through partial fills to completion.
// Reuses OrderSide, OrderType, OrderStatus from spp::quant::backtest.
// =========================================================================
using spp::quant::backtest::OrderSide;
using spp::quant::backtest::OrderType;
using spp::quant::backtest::OrderStatus;

struct Order {
    u64         id_               = 0;
    Date        created_;
    OrderSide   side_             = OrderSide::Buy;
    OrderType   type_             = OrderType::Market;
    f64         quantity_         = 0.0;
    f64         filled_quantity_  = 0.0;
    f64         avg_fill_price_   = 0.0;
    f64         limit_price_      = 0.0;    // for limit orders, 0 = market
    f64         stop_price_       = 0.0;    // for stop orders
    f64         visible_quantity_ = 0.0;    // for iceberg orders
    OrderStatus status_           = OrderStatus::New;
    Date        last_update_;

    // -------------------------------------------------------------------
    // remaining — unfilled quantity
    // -------------------------------------------------------------------
    [[nodiscard]] f64 remaining() const noexcept {
        return quantity_ - filled_quantity_;
    }

    // -------------------------------------------------------------------
    // is_done — terminal state?
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_done() const noexcept {
        return status_ == OrderStatus::Filled
            || status_ == OrderStatus::Cancelled
            || status_ == OrderStatus::Rejected;
    }

    // -------------------------------------------------------------------
    // is_active — still working?
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_active() const noexcept {
        return !is_done();
    }

    // -------------------------------------------------------------------
    // fill_pct — fraction filled
    // -------------------------------------------------------------------
    [[nodiscard]] f64 fill_pct() const noexcept {
        return quantity_ > 0.0 ? filled_quantity_ / quantity_ : 0.0;
    }

    SPP_RECORD(Order, SPP_FIELD(id_), SPP_FIELD(created_), SPP_FIELD(side_),
               SPP_FIELD(type_), SPP_FIELD(quantity_), SPP_FIELD(filled_quantity_),
               SPP_FIELD(avg_fill_price_), SPP_FIELD(limit_price_), SPP_FIELD(stop_price_),
               SPP_FIELD(visible_quantity_), SPP_FIELD(status_), SPP_FIELD(last_update_));
};

// =========================================================================
// OrderManager — CRUD for orders, indexed by id and symbol
// =========================================================================
struct OrderManager {
    u64 next_id_ = 1;

    Map<u64, Order>          orders_;            // id -> Order
    Map<u64, Vec<u64>>       orders_by_symbol_hash_;  // symbol hash -> order ids
    Vec<String>              symbol_storage_;    // owns symbol strings

    // -------------------------------------------------------------------
    // create_order — allocate an id, store, return the id
    // -------------------------------------------------------------------
    u64 create_order(OrderSide side, OrderType type, String_View symbol,
                      f64 quantity, f64 limit_price = 0.0, f64 stop_price = 0.0) {
        u64 id = next_id_++;

        Order order;
        order.id_          = id;
        order.created_     = Date::today();
        order.side_        = side;
        order.type_        = type;
        order.quantity_    = quantity;
        order.limit_price_ = limit_price;
        order.stop_price_  = stop_price;
        order.status_      = OrderStatus::New;
        order.last_update_ = order.created_;

        orders_.insert(id, spp::move(order));

        // Index by symbol hash for fast lookup
        u64 sym_hash = symbol.hash();
        auto opt = orders_by_symbol_hash_.try_get(sym_hash);
        if (opt.ok()) {
            (**opt).push(id);
        } else {
            Vec<u64> vec;
            vec.push(id);
            orders_by_symbol_hash_.insert(sym_hash, spp::move(vec));
        }

        // Store symbol string if not already present
        // (simplified: store every time; in production use a set)
        symbol_storage_.push(String{symbol});

        return id;
    }

    // -------------------------------------------------------------------
    // cancel_order — mark an order as cancelled
    // -------------------------------------------------------------------
    bool cancel_order(u64 id) {
        auto opt = orders_.try_get(id);
        if (!opt.ok()) return false;

        Order& order = **opt;
        if (order.is_done()) return false;

        order.status_      = OrderStatus::Cancelled;
        order.last_update_ = Date::today();
        return true;
    }

    // -------------------------------------------------------------------
    // on_fill — record a fill against an order
    // Returns false if the order is not found or already done.
    // -------------------------------------------------------------------
    bool on_fill(u64 id, f64 fill_qty, f64 fill_price) {
        auto opt = orders_.try_get(id);
        if (!opt.ok()) return false;

        Order& order = **opt;
        if (order.is_done()) return false;

        f64 remaining = order.remaining();
        f64 actual_qty = Math::min(fill_qty, remaining);

        // Update average fill price
        f64 total_cost = order.filled_quantity_ * order.avg_fill_price_
                       + actual_qty * fill_price;
        order.filled_quantity_ += actual_qty;
        if (order.filled_quantity_ > 0.0) {
            order.avg_fill_price_ = total_cost / order.filled_quantity_;
        }

        // Update status
        f64 unfilled = order.remaining();
        if (unfilled <= 1e-12) {
            order.status_ = OrderStatus::Filled;
        } else {
            order.status_ = OrderStatus::Partial;
        }

        order.last_update_ = Date::today();
        return true;
    }

    // -------------------------------------------------------------------
    // get_order — lookup by id
    // -------------------------------------------------------------------
    [[nodiscard]] Opt<Order&> get_order(u64 id) noexcept {
        auto opt = orders_.try_get(id);
        if (!opt.ok()) return {};
        return Opt<Order&>{**opt};
    }

    [[nodiscard]] Opt<const Order&> get_order(u64 id) const noexcept {
        auto opt = orders_.try_get(id);
        if (!opt.ok()) return {};
        return Opt<const Order&>{**opt};
    }

    // -------------------------------------------------------------------
    // orders_for_symbol — all order ids for a given symbol
    // -------------------------------------------------------------------
    [[nodiscard]] Slice<const u64> orders_for_symbol(String_View symbol) const noexcept {
        u64 sym_hash = symbol.hash();
        auto opt = orders_by_symbol_hash_.try_get(sym_hash);
        if (!opt.ok()) return Slice<const u64>{};
        return (**opt).slice();
    }

    // -------------------------------------------------------------------
    // pending_orders — ids of all New or Partial orders
    // -------------------------------------------------------------------
    [[nodiscard]] Vec<u64> pending_orders() const noexcept {
        Vec<u64> result;
        for (const auto& kv : orders_) {
            if (kv.second.is_active()) {
                result.push(kv.first);
            }
        }
        return result;
    }

    // -------------------------------------------------------------------
    // pending_count — count of active orders
    // -------------------------------------------------------------------
    [[nodiscard]] u64 pending_count() const noexcept {
        u64 count = 0;
        for (const auto& kv : orders_) {
            if (kv.second.is_active()) count++;
        }
        return count;
    }

    // -------------------------------------------------------------------
    // total_orders — total number of orders ever created
    // -------------------------------------------------------------------
    [[nodiscard]] u64 total_orders() const noexcept {
        return orders_.length();
    }

    // -------------------------------------------------------------------
    // reset — clear all state
    // -------------------------------------------------------------------
    void reset() noexcept {
        next_id_ = 1;
        orders_.clear();
        orders_by_symbol_hash_.clear();
        symbol_storage_.clear();
    }

    SPP_RECORD(OrderManager, SPP_FIELD(next_id_));
};

} // namespace spp::quant::execution

SPP_NAMED_RECORD(::spp::quant::execution::Order, "ExecutionOrder",
                 SPP_FIELD(id_), SPP_FIELD(created_), SPP_FIELD(side_),
                 SPP_FIELD(type_), SPP_FIELD(quantity_), SPP_FIELD(filled_quantity_),
                 SPP_FIELD(avg_fill_price_), SPP_FIELD(limit_price_),
                 SPP_FIELD(stop_price_), SPP_FIELD(visible_quantity_),
                 SPP_FIELD(status_), SPP_FIELD(last_update_));

SPP_NAMED_RECORD(::spp::quant::execution::OrderManager, "OrderManager",
                 SPP_FIELD(next_id_));
