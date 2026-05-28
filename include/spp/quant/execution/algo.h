#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/execution/order.h"
#include "spp/quant/backtest/event.h"       // FillEvent
#include "spp/quant/data/timeseries.h"

namespace spp::quant::execution {

using spp::quant::backtest::FillEvent;
using spp::quant::backtest::OrderSide;

// =========================================================================
// TWAP — Time-Weighted Average Price execution
//
// Splits a parent order into equal-sized child orders distributed evenly
// across the execution interval [start_time, end_time].
// =========================================================================
struct TWAP {
    Date        start_time_;
    Date        end_time_;
    u64         num_slices_        = 0;
    f64         total_quantity_    = 0.0;
    f64         quantity_per_slice_ = 0.0;
    OrderSide   side_              = OrderSide::Buy;
    String      symbol_;
    f64         limit_price_       = 0.0;  // 0 = aggressive (no limit)

    u64         slices_sent_       = 0;
    f64         filled_quantity_   = 0.0;

    // -------------------------------------------------------------------
    // create — factory for a TWAP schedule
    // -------------------------------------------------------------------
    static TWAP create(OrderSide side, String_View symbol, f64 quantity,
                       Date start, Date end, u64 slices, f64 limit = 0.0) noexcept {
        TWAP twap;
        twap.side_            = side;
        twap.symbol_          = String{symbol};
        twap.total_quantity_  = quantity;
        twap.start_time_      = start;
        twap.end_time_        = end;
        twap.num_slices_      = slices;
        twap.limit_price_     = limit;
        twap.quantity_per_slice_ = (slices > 0) ? quantity / static_cast<f64>(slices) : quantity;
        twap.slices_sent_     = 0;
        twap.filled_quantity_ = 0.0;
        return twap;
    }

    // -------------------------------------------------------------------
    // next_slice — generate the next child order for the current time
    // Returns empty Opt when no more slices remain.
    // -------------------------------------------------------------------
    [[nodiscard]] Opt<Order> next_slice(Date current_time) {
        if (is_complete()) return {};
        if (current_time < start_time_) return {};

        f64 remaining = remaining_quantity();
        if (remaining <= 0.0) return {};

        // Check if we're past the end time
        if (current_time > end_time_) {
            // Send remaining as final slice
            Order order;
            order.id_          = 0;  // caller assigns id
            order.created_     = current_time;
            order.side_        = side_;
            order.type_        = (limit_price_ > 0.0) ? OrderType::Limit : OrderType::Market;
            order.quantity_    = remaining;
            order.limit_price_ = limit_price_;
            order.status_      = OrderStatus::New;
            order.last_update_ = current_time;
            slices_sent_++;
            return Opt<Order>{spp::move(order)};
        }

        // Normal slice
        f64 slice_qty = Math::min(quantity_per_slice_, remaining);

        Order order;
        order.id_          = 0;
        order.created_     = current_time;
        order.side_        = side_;
        order.type_        = (limit_price_ > 0.0) ? OrderType::Limit : OrderType::Market;
        order.quantity_    = slice_qty;
        order.limit_price_ = limit_price_;
        order.status_      = OrderStatus::New;
        order.last_update_ = current_time;

        slices_sent_++;
        return Opt<Order>{spp::move(order)};
    }

    // -------------------------------------------------------------------
    // on_fill — notify TWAP of a fill on one of its child orders
    // -------------------------------------------------------------------
    void on_fill(f64 qty) noexcept {
        filled_quantity_ += qty;
    }

    // -------------------------------------------------------------------
    // slices_remaining — how many slices are still unsent
    // -------------------------------------------------------------------
    [[nodiscard]] u64 slices_remaining() const noexcept {
        if (slices_sent_ >= num_slices_) return 0;
        return num_slices_ - slices_sent_;
    }

    // -------------------------------------------------------------------
    // remaining_quantity — unfilled quantity
    // -------------------------------------------------------------------
    [[nodiscard]] f64 remaining_quantity() const noexcept {
        f64 rem = total_quantity_ - filled_quantity_;
        return rem > 0.0 ? rem : 0.0;
    }

    // -------------------------------------------------------------------
    // is_complete — all quantity filled or all slices sent
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_complete() const noexcept {
        return remaining_quantity() <= 1e-12 || slices_sent_ >= num_slices_;
    }

    // -------------------------------------------------------------------
    // progress — fraction of total quantity filled
    // -------------------------------------------------------------------
    [[nodiscard]] f64 progress() const noexcept {
        return total_quantity_ > 0.0 ? filled_quantity_ / total_quantity_ : 1.0;
    }
};

// =========================================================================
// VWAP — Volume-Weighted Average Price execution
//
// Similar to TWAP but sizes child orders proportionally to a historical
// volume profile. The schedule_ is precomputed from volume_profile_ at
// creation time.
// =========================================================================
struct VWAP {
    Date      start_time_;
    Date      end_time_;
    f64       total_quantity_    = 0.0;
    OrderSide side_              = OrderSide::Buy;
    String    symbol_;

    Vec<Pair<Date, f64>> schedule_;       // precomputed: (date, quantity)
    u64                  schedule_idx_    = 0;
    f64                  filled_quantity_ = 0.0;

    // -------------------------------------------------------------------
    // create — factory using a historical volume time series
    //
    // The volume time series should contain daily volume data.
    // The schedule distributes total_quantity proportionally to the
    // historical volume on each date in [start, end].
    // -------------------------------------------------------------------
    static VWAP create(OrderSide side, String_View symbol, f64 quantity,
                       Date start, Date end,
                       const TimeSeries<f64>& historical_volume) noexcept {
        VWAP vwap;
        vwap.side_            = side;
        vwap.symbol_          = String{symbol};
        vwap.total_quantity_  = quantity;
        vwap.start_time_      = start;
        vwap.end_time_        = end;

        // Aggregate total volume in the date range
        f64 total_vol = 0.0;
        Vec<Pair<Date, f64>> vol_pairs;

        auto dates  = historical_volume.dates();
        auto values = historical_volume.values();
        u64 n = dates.length();

        for (u64 i = 0; i < n; i++) {
            if (dates[i] >= start && dates[i] <= end) {
                f64 vol = values[i];
                if (vol > 0.0) {
                    total_vol += vol;
                    vol_pairs.push(Pair<Date, f64>{dates[i], vol});
                }
            }
            if (dates[i] > end) break;
        }

        // Distribute quantity proportionally
        if (total_vol > 0.0 && !vol_pairs.empty()) {
            for (u64 i = 0; i < vol_pairs.length(); i++) {
                f64 frac = vol_pairs[i].second / total_vol;
                f64 alloc = frac * quantity;
                vwap.schedule_.push(Pair<Date, f64>{vol_pairs[i].first, alloc});
            }
        } else if (!vol_pairs.empty()) {
            // Equal distribution fallback
            f64 equal = quantity / static_cast<f64>(vol_pairs.length());
            for (u64 i = 0; i < vol_pairs.length(); i++) {
                vwap.schedule_.push(Pair<Date, f64>{vol_pairs[i].first, equal});
            }
        }

        vwap.schedule_idx_    = 0;
        vwap.filled_quantity_ = 0.0;

        return vwap;
    }

    // -------------------------------------------------------------------
    // next_slice — generate next child order for the current time
    // -------------------------------------------------------------------
    [[nodiscard]] Opt<Order> next_slice(Date current_time) {
        if (is_complete()) return {};

        // Skip past schedule entries before current_time
        while (schedule_idx_ < schedule_.length()
               && schedule_[schedule_idx_].first < current_time) {
            schedule_idx_++;
        }

        if (schedule_idx_ >= schedule_.length()) return {};

        // Match exactly or take the next entry on/after current_time
        if (schedule_[schedule_idx_].first > current_time) return {};

        f64 qty = schedule_[schedule_idx_].second;
        schedule_idx_++;

        f64 remaining = remaining_quantity();
        if (qty > remaining) qty = remaining;
        if (qty <= 0.0) return {};

        Order order;
        order.id_          = 0;
        order.created_     = current_time;
        order.side_        = side_;
        order.type_        = OrderType::Market;
        order.quantity_    = qty;
        order.status_      = OrderStatus::New;
        order.last_update_ = current_time;

        return Opt<Order>{spp::move(order)};
    }

    // -------------------------------------------------------------------
    // on_fill — notify of a fill on a child order
    // -------------------------------------------------------------------
    void on_fill(f64 qty) noexcept {
        filled_quantity_ += qty;
    }

    // -------------------------------------------------------------------
    // remaining_quantity
    // -------------------------------------------------------------------
    [[nodiscard]] f64 remaining_quantity() const noexcept {
        f64 rem = total_quantity_ - filled_quantity_;
        return rem > 0.0 ? rem : 0.0;
    }

    // -------------------------------------------------------------------
    // is_complete
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_complete() const noexcept {
        return remaining_quantity() <= 1e-12
            || schedule_idx_ >= schedule_.length();
    }

    // -------------------------------------------------------------------
    // progress
    // -------------------------------------------------------------------
    [[nodiscard]] f64 progress() const noexcept {
        return total_quantity_ > 0.0 ? filled_quantity_ / total_quantity_ : 1.0;
    }
};

// =========================================================================
// Iceberg — slice a large order into visible chunks
//
// Only displays `visible_quantity_` on the order book at a time.
// When the visible portion is filled, a new slice is automatically
// revealed until the total quantity is exhausted.
// =========================================================================
struct Iceberg {
    f64       total_quantity_   = 0.0;
    f64       visible_quantity_ = 0.0;
    f64       filled_visible_   = 0.0;  // how much of the current visible slice is filled
    f64       filled_total_     = 0.0;  // total filled so far
    OrderSide side_             = OrderSide::Buy;
    String    symbol_;
    f64       limit_price_      = 0.0;
    u64       slices_created_   = 0;

    // -------------------------------------------------------------------
    // create — factory
    // -------------------------------------------------------------------
    static Iceberg create(OrderSide side, String_View symbol,
                           f64 total_qty, f64 visible_qty,
                           f64 limit = 0.0) noexcept {
        Iceberg ib;
        ib.side_              = side;
        ib.symbol_            = String{symbol};
        ib.total_quantity_    = total_qty;
        ib.visible_quantity_  = visible_qty;
        ib.limit_price_       = limit;
        ib.filled_visible_    = 0.0;
        ib.filled_total_      = 0.0;
        ib.slices_created_    = 0;
        return ib;
    }

    // -------------------------------------------------------------------
    // next_slice — generate the next visible chunk
    // Returns empty when the visible portion still has quantity or total
    // is exhausted.
    // -------------------------------------------------------------------
    [[nodiscard]] Opt<Order> next_slice() {
        // If current visible slice still has unfilled quantity, don't reveal more
        if (filled_visible_ < visible_quantity_ && slices_created_ > 0) {
            return {};
        }

        f64 remaining = remaining();
        if (remaining <= 0.0) return {};

        f64 slice_qty = Math::min(visible_quantity_, remaining);

        Order order;
        order.id_              = 0;
        order.created_         = Date::today();
        order.side_            = side_;
        order.type_            = (limit_price_ > 0.0) ? OrderType::Limit : OrderType::Market;
        order.quantity_        = slice_qty;
        order.limit_price_     = limit_price_;
        order.visible_quantity_ = visible_quantity_;
        order.status_          = OrderStatus::New;
        order.last_update_     = order.created_;

        filled_visible_ = 0.0;
        slices_created_++;

        return Opt<Order>{spp::move(order)};
    }

    // -------------------------------------------------------------------
    // on_fill — notify of a fill
    // -------------------------------------------------------------------
    void on_fill(f64 qty) noexcept {
        filled_visible_ += qty;
        filled_total_   += qty;

        // If visible portion is fully filled, reset for next slice
        if (filled_visible_ >= visible_quantity_) {
            filled_visible_ = 0.0;
        }
    }

    // -------------------------------------------------------------------
    // remaining — total quantity remaining
    // -------------------------------------------------------------------
    [[nodiscard]] f64 remaining() const noexcept {
        f64 rem = total_quantity_ - filled_total_;
        return rem > 0.0 ? rem : 0.0;
    }

    // -------------------------------------------------------------------
    // is_complete — all quantity exhausted
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_complete() const noexcept {
        return remaining() <= 1e-12;
    }

    // -------------------------------------------------------------------
    // progress
    // -------------------------------------------------------------------
    [[nodiscard]] f64 progress() const noexcept {
        return total_quantity_ > 0.0 ? filled_total_ / total_quantity_ : 1.0;
    }
};

// =========================================================================
// ImplementationShortfall — measures execution quality
//
// Implementation shortfall = (avg_fill_price - arrival_price) / arrival_price
//   for buys (positive = paid more than arrival)
//   for sells (negative = received less than arrival, stored as positive cost)
//
// Also computes the Almgren-Chriss optimal trading trajectory.
// =========================================================================
struct ImplementationShortfall {
    Date      arrival_time_;
    f64       arrival_price_     = 0.0;
    f64       target_quantity_   = 0.0;
    OrderSide side_              = OrderSide::Buy;
    String    symbol_;
    f64       urgency_           = 0.5;   // higher = faster execution
    f64       risk_aversion_     = 1e-4;  // higher = less variance tolerated
    f64       daily_volatility_  = 0.2;

    // -------------------------------------------------------------------
    // optimal_schedule — AC optimal trajectory
    //
    // Returns a vector of (date, quantity) pairs representing the optimal
    // trading schedule to minimize implementation shortfall.
    //
    // The AC solution (for linear temporary + permanent impact):
    //   v(t) = X * kappa * cosh(kappa*(1-t/T)) / sinh(kappa)
    // where kappa^2 = lambda * sigma^2 * X^2 / (eta * V^2)
    // and lambda = risk_aversion, X = target_quantity, sigma = volatility,
    // eta = temporary impact coefficient, V = market volume.
    //
    // The schedule is discretized into `num_intervals` buckets.
    // -------------------------------------------------------------------
    [[nodiscard]] Vec<Pair<Date, f64>> optimal_schedule(u64 num_intervals) const noexcept {
        Vec<Pair<Date, f64>> schedule;
        if (num_intervals == 0 || target_quantity_ <= 0.0) return schedule;

        // Simplified AC schedule: use urgency_ to determine front-loading
        // urgency near 0: uniform (TWAP-like)
        // urgency near 1: heavily front-loaded

        f64 remaining = target_quantity_;
        f64 total_weight = 0.0;

        // Exponential decay weight: w_i = exp(-urgency * i / N)
        Vec<f64> weights;
        weights.reserve(num_intervals);

        for (u64 i = 0; i < num_intervals; i++) {
            f64 t = static_cast<f64>(i) / static_cast<f64>(num_intervals);
            f64 w = Math::exp(-urgency_ * t);
            weights.push(w);
            total_weight += w;
        }

        if (total_weight <= 0.0) {
            // Degenerate: equal distribution
            f64 equal = target_quantity_ / static_cast<f64>(num_intervals);
            for (u64 i = 0; i < num_intervals; i++) {
                Date d = arrival_time_ + static_cast<i32>(i);
                schedule.push(Pair<Date, f64>{d, equal});
            }
            return schedule;
        }

        // Normalize and build schedule
        for (u64 i = 0; i < num_intervals; i++) {
            f64 frac = weights[i] / total_weight;
            f64 alloc = frac * target_quantity_;
            if (alloc > remaining) alloc = remaining;
            remaining -= alloc;

            Date d = arrival_time_ + static_cast<i32>(i);
            schedule.push(Pair<Date, f64>{d, alloc});

            if (remaining <= 0.0) break;
        }

        return schedule;
    }

    // -------------------------------------------------------------------
    // expected_cost — estimate of expected implementation shortfall
    //
    // Uses a simple square-root market impact model:
    //   cost ~ spread_pct + impact_pct * sqrt(Q / ADV) * sigma
    // -------------------------------------------------------------------
    [[nodiscard]] f64 expected_cost(f64 daily_volume = 1e6,
                                     f64 spread_pct = 0.0005,
                                     f64 impact_coef = 0.1) const noexcept {
        if (arrival_price_ <= 0.0 || target_quantity_ <= 0.0 || daily_volume <= 0.0) {
            return 0.0;
        }

        f64 participation = target_quantity_ / daily_volume;
        f64 impact_pct = impact_coef * daily_volatility_ * Math::sqrt(participation);
        f64 total_cost_pct = spread_pct + impact_pct;

        return total_cost_pct * arrival_price_ * target_quantity_;
    }

    // -------------------------------------------------------------------
    // realized_cost — actual implementation shortfall from fills
    //
    // For buys:  cost = avg_fill - arrival (positive = paid more)
    // For sells: cost = arrival - avg_fill (positive = received less)
    // Returns total cost in price units.
    // -------------------------------------------------------------------
    [[nodiscard]] f64 realized_cost(const Vec<FillEvent>& fills) const noexcept {
        if (fills.empty()) return 0.0;

        f64 total_qty   = 0.0;
        f64 total_value = 0.0;

        for (u64 i = 0; i < fills.length(); i++) {
            const FillEvent& f = fills[i];
            total_qty   += f.quantity_;
            total_value += f.quantity_ * f.price_;
        }

        if (total_qty <= 0.0) return 0.0;

        f64 avg_fill = total_value / total_qty;

        if (side_ == OrderSide::Buy) {
            return (avg_fill - arrival_price_) * total_qty;
        } else {
            return (arrival_price_ - avg_fill) * total_qty;
        }
    }
};

} // namespace spp::quant::execution
