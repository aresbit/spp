#pragma once

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/containers/vec.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/numeric/math.h>
#include <spp/quant/strategy/types.h>

namespace spp::quant::strategy {

// ============================================================
// Order_Relay — Order Mirroring / Copy-Trading
//
// Listens to orders from a source strategy and forwards transformed
// copies to a target account. Used for:
//   - Copy-trading (copy signals from master account)
//   - Order splitting (one signal → multiple accounts)
//   - Multiplier-based position scaling
// ============================================================

template <typename A = Mdefault>
struct Order_Relay {
    /// Strategy ID to copy orders FROM.
    String<A> source_id;
    /// Account / strategy ID to forward orders TO.
    String<A> target_id;
    /// Volume multiplier (e.g., 2.0 = double the source size).
    f64 lot_multiplier = 1.0;
    /// Maximum position size in target account (risk limit).
    f64 max_position_volume = 0.0;  // 0 = no limit
    /// Whether to relay orders (false = pause relaying).
    bool active = true;

    /// History of relayed orders for audit trail.
    Vec<Order, A> relayed_orders;

    Order_Relay() noexcept = default;

    Order_Relay(String<A> src, String<A> tgt, f64 multiplier = 1.0) noexcept
        : source_id(spp::move(src)), target_id(spp::move(tgt)),
          lot_multiplier(multiplier) {
    }

    /// Transform a source order into a target order.
    /// Applies lot_multiplier to volume and prefixes order_id with target.
    auto relay_order(const Order& order) -> Order {
        Order relayed;
        relayed.order_id = _make_relayed_id(order.order_id.view());
        relayed.code = order.code.clone();
        relayed.direction = order.direction;
        relayed.offset = order.offset;
        relayed.price = order.price;
        relayed.volume = order.volume * lot_multiplier;
        relayed.time = order.time;
        relayed.status = 0; // reset to pending

        // Apply max position size cap
        if (max_position_volume > 0.0 && relayed.volume > max_position_volume) {
            relayed.volume = max_position_volume;
        }

        relayed_orders.push(Order{relayed.order_id.clone(),
                                   relayed.code.clone(),
                                   relayed.direction,
                                   relayed.offset,
                                   relayed.price,
                                   relayed.volume,
                                   relayed.time,
                                   relayed.status});

        return relayed;
    }

    /// Virtual hook: called when a source order is received.
    /// Override this to add custom filtering or transformation logic.
    virtual void on_order(const Order& order) {
        if (!active) return;

        // Default: silently relay all orders
        // Derived classes can filter by code, direction, time, etc.
    }

    /// Send the relayed order (to be called by the event loop).
    auto send_order(const Order& order) -> void {
        Order relayed = relay_order(order);
        on_order(relayed);
    }

    /// Start relaying: set active flag.
    auto start() -> void {
        active = true;
    }

    /// Pause relaying: keep state but stop forwarding.
    auto pause() -> void {
        active = false;
    }

    /// Stop and reset: clear relay history, set inactive.
    auto stop() -> void {
        active = false;
        relayed_orders = Vec<Order, A>{};
    }

    /// Check if this relay would forward a given order.
    /// Can be overridden for custom filtering.
    auto should_relay(const Order& order) const -> bool {
        if (!active) return false;
        if (lot_multiplier <= 0.0) return false;
        if (order.volume <= 0.0) return false;
        if (order.status != 0) return false; // only relay pending orders
        return true;
    }

    /// Get the count of relayed orders.
    auto relay_count() const -> u64 {
        return relayed_orders.length();
    }

    /// Get total relayed volume across all orders.
    auto total_relayed_volume() const -> f64 {
        f64 total = 0.0;
        for (u64 i = 0; i < relayed_orders.length(); i++) {
            total += relayed_orders[i].volume;
        }
        return total;
    }

private:
    u64 counter_ = 0;

    auto _make_relayed_id(String_View original_id) -> String<A> {
        char buf[64];
        const char* src_id_ptr = reinterpret_cast<const char*>(original_id.data());
        u64 src_len = original_id.length();
        if (src_len > 32) src_len = 32; // truncate

        (void)Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf),
                       "RLY_%.*s_%llu",
                       static_cast<int>(src_len), src_id_ptr,
                       static_cast<unsigned long long>(++counter_));

        u64 len = Libc::strlen(buf);
        String<A> result(len);
        result.set_length(len);
        Libc::memcpy(result.data(), buf, len);
        return result;
    }
};

} // namespace spp::quant::strategy
