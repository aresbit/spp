#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/core/types.h"      // unified event types
#include "spp/quant/base/date.h"
#include "spp/quant/portfolio/position.h"

namespace spp::quant {

// =========================================================================
// Strategy — abstract base class for all trading strategies
//
// Event types (MarketEvent, SignalEvent, OrderEvent, FillEvent) are the
// unified definitions from core/types.h.  Field names:
//   symbol_    (was instrument_id_)
//   date_      (was timestamp_)
//   price_     (was limit_price_ / fill_price_)
//
// SignalDirection uses: Short / Flat / Long  (was Sell / Flat / Buy)
// =========================================================================

struct Strategy {
    String_View  name_      = "Unnamed"_v;
    PositionBook positions_;
    f64          cash_      = 0.0;
    f64          init_cash_ = 0.0;

    // ---- Event handlers (pure virtual) ----------------------------------

    /// Called for each new market data event.
    /// Returns an optional signal if the strategy wants to trade.
    virtual Opt<SignalEvent> on_market_data(const MarketEvent& event) = 0;

    /// Called when a signal is generated — convert signal into orders.
    virtual Vec<OrderEvent> on_signal(const SignalEvent& signal) = 0;

    /// Called when an order is filled — update internal state.
    virtual void on_fill(const FillEvent& fill) = 0;

    /// Called when a timer fires (e.g. daily close, weekly rebalance).
    /// Default: no-op.
    virtual Opt<SignalEvent> on_timer(Date /*current_date*/) { return {}; }

    // ---- Risk limits (virtual, can be overridden) ------------------------

    /// Maximum position as fraction of total capital (per instrument).
    [[nodiscard]] virtual f64 max_position_pct() const noexcept { return 1.0; }

    /// Maximum gross leverage (gross exposure / capital).
    [[nodiscard]] virtual f64 max_leverage() const noexcept { return 1.0; }

    /// Maximum number of orders per trading day.
    [[nodiscard]] virtual u64 max_orders_per_day() const noexcept { return 100; }

    virtual ~Strategy() = default;

    // ---- Position sizing helpers -----------------------------------------

    /// Total current exposure (sum of |qty * price| across all positions).
    /// Requires external price data; returns 0.0 without it.
    [[nodiscard]] f64 current_exposure() const noexcept {
        f64 exp = 0.0;
        for (u64 i = 0; i < positions_.size(); i++) {
            const Position& p = positions_.positions_[i];
            f64 val = p.quantity_ * p.entry_price_;
            exp += (val >= 0.0) ? val : -val;
        }
        return exp;
    }

    /// Available capital = cash + unrealized PnL (approximate).
    [[nodiscard]] f64 available_capital() const noexcept { return cash_; }

    /// Number of non-zero positions.
    [[nodiscard]] u64 position_count() const noexcept {
        u64 cnt = 0;
        for (u64 i = 0; i < positions_.size(); i++) {
            if (positions_.positions_[i].quantity_ != 0.0) cnt++;
        }
        return cnt;
    }

    SPP_RECORD(Strategy, SPP_FIELD(name_));
};

// =========================================================================
// MACrossover — simple moving average crossover strategy
// =========================================================================
//
// Computes two simple moving averages on close prices.
// Signal generation:
//   - Long  when fast_ma crosses above slow_ma (golden cross)
//   - Short when fast_ma crosses below slow_ma (death cross)
//
// Uses position tracking to avoid duplicate signals:
//   - Only generate Long  when currently flat/short
//   - Only generate Short when currently long

struct MACrossover : Strategy {
    u64 fast_window_       = 20;
    u64 slow_window_       = 50;
    f64 position_size_pct_ = 1.0;  ///< fraction of capital per trade

    // Internal state
    Vec<f64>        price_history_;
    SignalDirection current_direction_ = SignalDirection::Flat;

    Opt<SignalEvent> on_market_data(const MarketEvent& event) override {
        // Accumulate price history
        f64 price = event.close_;
        price_history_.push(price);

        u64 n = price_history_.length();
        if (n < slow_window_ + 1) return {};  // not enough data

        // Compute fast and slow moving averages for the current bar
        f64 fast_ma_curr = 0.0;
        for (u64 i = n - fast_window_; i < n; i++)
            fast_ma_curr += price_history_[i];
        fast_ma_curr /= static_cast<f64>(fast_window_);

        f64 slow_ma_curr = 0.0;
        for (u64 i = n - slow_window_; i < n; i++)
            slow_ma_curr += price_history_[i];
        slow_ma_curr /= static_cast<f64>(slow_window_);

        // Compute previous bar's MAs (for crossover detection)
        f64 fast_ma_prev = 0.0;
        for (u64 i = n - fast_window_ - 1; i < n - 1; i++)
            fast_ma_prev += price_history_[i];
        fast_ma_prev /= static_cast<f64>(fast_window_);

        f64 slow_ma_prev = 0.0;
        for (u64 i = n - slow_window_ - 1; i < n - 1; i++)
            slow_ma_prev += price_history_[i];
        slow_ma_prev /= static_cast<f64>(slow_window_);

        // Crossover detection
        bool fast_above_slow_curr = fast_ma_curr > slow_ma_curr;
        bool fast_above_slow_prev = fast_ma_prev > slow_ma_prev;

        if (fast_above_slow_curr && !fast_above_slow_prev) {
            // Golden cross: fast crossed above slow -> Long
            if (current_direction_ != SignalDirection::Long) {
                SignalEvent sig;
                sig.symbol_    = event.symbol_;
                sig.direction_ = SignalDirection::Long;
                sig.strength_  = 1.0;
                sig.confidence_ = 1.0;
                sig.date_      = event.date_;
                return Opt{spp::move(sig)};
            }
        } else if (!fast_above_slow_curr && fast_above_slow_prev) {
            // Death cross: fast crossed below slow -> Short
            if (current_direction_ != SignalDirection::Short) {
                SignalEvent sig;
                sig.symbol_    = event.symbol_;
                sig.direction_ = SignalDirection::Short;
                sig.strength_  = -1.0;
                sig.confidence_ = 1.0;
                sig.date_      = event.date_;
                return Opt{spp::move(sig)};
            }
        }

        return {};
    }

    Vec<OrderEvent> on_signal(const SignalEvent& signal) override {
        Vec<OrderEvent> orders;

        if (signal.direction_ == SignalDirection::Flat) return orders;

        // Determine target position
        f64 target_qty = 0.0;
        f64 capital = available_capital();
        if (capital <= 0.0) return orders;

        // Position size = position_size_pct_ * capital / price
        f64 notional = capital * position_size_pct_;
        if (signal.direction_ == SignalDirection::Long) {
            target_qty = notional / signal.strength_;  // placeholder: need actual price
        } else {
            target_qty = -notional;
        }

        // Determine what we currently hold
        f64 current_qty = 0.0;
        auto pos_opt = positions_.find(signal.symbol_);
        if (pos_opt.ok()) {
            current_qty = (*pos_opt)->quantity_;
        }

        f64 delta = target_qty - current_qty;
        if (Math::abs(delta) < 1e-10) return orders;

        OrderEvent order;
        order.symbol_   = signal.symbol_;
        order.date_     = signal.date_;
        order.side_     = (delta > 0.0) ? OrderSide::Buy : OrderSide::Sell;
        order.quantity_ = (delta > 0.0) ? delta : -delta;
        order.price_    = 0.0;  // market order
        orders.push(spp::move(order));

        current_direction_ = (target_qty > 0.0) ? SignalDirection::Long
                          : (target_qty < 0.0) ? SignalDirection::Short
                          : SignalDirection::Flat;

        return orders;
    }

    void on_fill(const FillEvent& fill) override {
        // Update position and cash
        f64 signed_qty = fill.signed_quantity();
        f64 cost = fill.price_ * fill.quantity_ + fill.commission_;

        auto pos_opt = positions_.find(fill.symbol_);
        if (pos_opt.ok()) {
            Position& pos = **pos_opt;
            if (pos.quantity_ + signed_qty == 0.0) {
                // Position closed
                positions_.remove(fill.symbol_);
            } else {
                // Average into entry price
                f64 new_qty = pos.quantity_ + signed_qty;
                f64 new_cost_basis = pos.quantity_ * pos.entry_price_ + signed_qty * fill.price_;
                pos.quantity_ = new_qty;
                pos.entry_price_ = (new_qty != 0.0) ? new_cost_basis / new_qty : 0.0;
            }
        } else if (signed_qty != 0.0) {
            Position new_pos;
            new_pos.instrument_id_ = fill.symbol_;
            new_pos.quantity_      = signed_qty;
            new_pos.entry_price_   = fill.price_;
            new_pos.entry_date_    = fill.date_;
            positions_.add(spp::move(new_pos));
        }

        if (fill.side_ == OrderSide::Buy)
            cash_ -= cost;
        else
            cash_ += fill.price_ * fill.quantity_ - fill.commission_;
    }

    SPP_RECORD(MACrossover, SPP_FIELD(fast_window_), SPP_FIELD(slow_window_));
};

// =========================================================================
// MeanReversion — Bollinger Band / Z-score based mean reversion
// =========================================================================
//
// Computes rolling z-score: z = (price - mean) / std
// Entry: when |z| > entry_z_score_  (price is extreme)
// Exit:  when |z| < exit_z_score_   (price has reverted)

struct MeanReversion : Strategy {
    u64 lookback_          = 20;
    f64 entry_z_score_     = 2.0;   ///< enter when |z| exceeds this
    f64 exit_z_score_      = 0.5;   ///< exit when |z| drops below this
    f64 position_size_pct_ = 1.0;

    Vec<f64> price_history_;
    SignalDirection current_direction_ = SignalDirection::Flat;

    /// Compute rolling z-score for the latest price
    [[nodiscard]] f64 compute_z_score() const noexcept {
        u64 n = price_history_.length();
        if (n < lookback_) return 0.0;

        // Compute mean and std of the lookback window
        f64 sum = 0.0;
        u64 start = n - lookback_;
        for (u64 i = start; i < n; i++)
            sum += price_history_[i];
        f64 mean = sum / static_cast<f64>(lookback_);

        f64 sum_sq = 0.0;
        for (u64 i = start; i < n; i++) {
            f64 diff = price_history_[i] - mean;
            sum_sq += diff * diff;
        }
        f64 std = Math::sqrt(sum_sq / static_cast<f64>(lookback_ - 1));
        if (std < 1e-15) return 0.0;

        f64 current = price_history_[n - 1];
        return (current - mean) / std;
    }

    Opt<SignalEvent> on_market_data(const MarketEvent& event) override {
        price_history_.push(event.close_);

        if (price_history_.length() < lookback_ + 1) return {};

        f64 z = compute_z_score();

        // Entry logic: price is extreme relative to its recent history
        if (current_direction_ == SignalDirection::Flat) {
            if (z > entry_z_score_) {
                // Price is too high -> sell short (mean-revert down)
                SignalEvent sig;
                sig.symbol_    = event.symbol_;
                sig.direction_ = SignalDirection::Short;
                sig.strength_  = -Math::min(1.0, (z - entry_z_score_) / entry_z_score_);
                sig.confidence_ = Math::min(1.0, Math::abs(z) / (2.0 * entry_z_score_));
                sig.date_      = event.date_;
                return Opt{spp::move(sig)};
            } else if (z < -entry_z_score_) {
                // Price is too low -> buy (mean-revert up)
                SignalEvent sig;
                sig.symbol_    = event.symbol_;
                sig.direction_ = SignalDirection::Long;
                sig.strength_  = Math::min(1.0, (-z - entry_z_score_) / entry_z_score_);
                sig.confidence_ = Math::min(1.0, Math::abs(z) / (2.0 * entry_z_score_));
                sig.date_      = event.date_;
                return Opt{spp::move(sig)};
            }
        }

        // Exit logic: price has reverted to the mean
        if (current_direction_ != SignalDirection::Flat) {
            if (Math::abs(z) < exit_z_score_) {
                SignalEvent sig;
                sig.symbol_    = event.symbol_;
                sig.direction_ = SignalDirection::Flat;  // exit
                sig.strength_  = 0.0;
                sig.confidence_ = 1.0;
                sig.date_      = event.date_;
                return Opt{spp::move(sig)};
            }
        }

        return {};
    }

    Vec<OrderEvent> on_signal(const SignalEvent& signal) override {
        Vec<OrderEvent> orders;
        if (signal.direction_ == SignalDirection::Flat) {
            // Exit existing position
            for (u64 i = 0; i < positions_.size(); i++) {
                const Position& p = positions_.positions_[i];
                if (p.quantity_ == 0.0) continue;
                OrderEvent order;
                order.symbol_   = p.instrument_id_;
                order.date_     = signal.date_;
                order.side_     = (p.quantity_ > 0.0) ? OrderSide::Sell : OrderSide::Buy;
                order.quantity_ = Math::abs(p.quantity_);
                order.price_    = 0.0;
                orders.push(spp::move(order));
            }
            current_direction_ = SignalDirection::Flat;
            return orders;
        }

        f64 capital = available_capital();
        if (capital <= 0.0) return orders;

        f64 notional = capital * position_size_pct_;

        OrderEvent order;
        order.symbol_   = signal.symbol_;
        order.date_     = signal.date_;
        if (signal.direction_ == SignalDirection::Long) {
            order.side_     = OrderSide::Buy;
            order.quantity_ = notional;  // placeholder, scaled by price at execution
        } else {
            order.side_     = OrderSide::Sell;
            order.quantity_ = notional;
        }
        order.price_ = 0.0;
        orders.push(spp::move(order));

        current_direction_ = signal.direction_;
        return orders;
    }

    void on_fill(const FillEvent& fill) override {
        f64 signed_qty = fill.signed_quantity();
        f64 cost = fill.price_ * fill.quantity_ + fill.commission_;

        auto pos_opt = positions_.find(fill.symbol_);
        if (pos_opt.ok()) {
            Position& pos = **pos_opt;
            if (pos.quantity_ + signed_qty == 0.0) {
                positions_.remove(fill.symbol_);
            } else {
                f64 new_qty = pos.quantity_ + signed_qty;
                f64 new_cost_basis = pos.quantity_ * pos.entry_price_ + signed_qty * fill.price_;
                pos.quantity_ = new_qty;
                pos.entry_price_ = (new_qty != 0.0) ? new_cost_basis / new_qty : 0.0;
            }
        } else if (signed_qty != 0.0) {
            Position new_pos;
            new_pos.instrument_id_ = fill.symbol_;
            new_pos.quantity_      = signed_qty;
            new_pos.entry_price_   = fill.price_;
            new_pos.entry_date_    = fill.date_;
            positions_.add(spp::move(new_pos));
        }

        if (fill.side_ == OrderSide::Buy)
            cash_ -= cost;
        else
            cash_ += fill.price_ * fill.quantity_ - fill.commission_;
    }

    SPP_RECORD(MeanReversion, SPP_FIELD(lookback_), SPP_FIELD(entry_z_score_),
               SPP_FIELD(exit_z_score_));
};

// =========================================================================
// Momentum — time-series momentum strategy
// =========================================================================
//
// Computes lookback return: r = (P_t - P_{t-lookback}) / P_{t-lookback}
// Entry: when r > min_return_pct_ (positive momentum -> Long)
//        when r < -min_return_pct_ (negative momentum -> Short)
// Hold for holding_period_ days, then exit and re-evaluate.

struct Momentum : Strategy {
    u64 lookback_          = 60;   ///< formation period (bars)
    u64 holding_period_    = 20;   ///< how long to hold after entry
    u64 days_held_         = 0;    ///< bars since last entry
    f64 min_return_pct_    = 0.05; ///< 5% minimum lookback return to trigger
    f64 position_size_pct_ = 1.0;

    Vec<f64> price_history_;
    SignalDirection current_direction_ = SignalDirection::Flat;

    /// Compute lookback return (fractional)
    [[nodiscard]] f64 lookback_return() const noexcept {
        u64 n = price_history_.length();
        if (n < lookback_ + 1) return 0.0;
        f64 p_curr = price_history_[n - 1];
        f64 p_prev = price_history_[n - 1 - lookback_];
        if (Math::abs(p_prev) < 1e-15) return 0.0;
        return (p_curr - p_prev) / p_prev;
    }

    Opt<SignalEvent> on_market_data(const MarketEvent& event) override {
        price_history_.push(event.close_);

        u64 n = price_history_.length();
        if (n < lookback_ + 1) return {};

        // If currently holding, check if holding period has elapsed
        if (current_direction_ != SignalDirection::Flat) {
            days_held_++;
            if (days_held_ >= holding_period_) {
                // Exit position
                SignalEvent sig;
                sig.symbol_    = event.symbol_;
                sig.direction_ = SignalDirection::Flat;
                sig.strength_  = 0.0;
                sig.confidence_ = 1.0;
                sig.date_      = event.date_;
                return Opt{spp::move(sig)};
            }
            return {};  // still holding
        }

        // Evaluate momentum signal
        f64 ret = lookback_return();
        if (ret > min_return_pct_) {
            // Positive momentum: go long
            SignalEvent sig;
            sig.symbol_    = event.symbol_;
            sig.direction_ = SignalDirection::Long;
            sig.strength_  = Math::min(1.0, ret / min_return_pct_);
            sig.confidence_ = Math::min(1.0, Math::abs(ret) / (2.0 * min_return_pct_));
            sig.date_      = event.date_;
            return Opt{spp::move(sig)};
        } else if (ret < -min_return_pct_) {
            // Negative momentum: go short
            SignalEvent sig;
            sig.symbol_    = event.symbol_;
            sig.direction_ = SignalDirection::Short;
            sig.strength_  = -Math::min(1.0, -ret / min_return_pct_);
            sig.confidence_ = Math::min(1.0, Math::abs(ret) / (2.0 * min_return_pct_));
            sig.date_      = event.date_;
            return Opt{spp::move(sig)};
        }

        return {};
    }

    Vec<OrderEvent> on_signal(const SignalEvent& signal) override {
        Vec<OrderEvent> orders;

        if (signal.direction_ == SignalDirection::Flat) {
            // Exit existing positions
            for (u64 i = 0; i < positions_.size(); i++) {
                const Position& p = positions_.positions_[i];
                if (p.quantity_ == 0.0) continue;
                OrderEvent order;
                order.symbol_   = p.instrument_id_;
                order.date_     = signal.date_;
                order.side_     = (p.quantity_ > 0.0) ? OrderSide::Sell : OrderSide::Buy;
                order.quantity_ = Math::abs(p.quantity_);
                order.price_    = 0.0;
                orders.push(spp::move(order));
            }
            current_direction_ = SignalDirection::Flat;
            days_held_ = 0;
            return orders;
        }

        f64 capital = available_capital();
        if (capital <= 0.0) return orders;

        f64 notional = capital * position_size_pct_;

        OrderEvent order;
        order.symbol_   = signal.symbol_;
        order.date_     = signal.date_;
        if (signal.direction_ == SignalDirection::Long) {
            order.side_     = OrderSide::Buy;
            order.quantity_ = notional;
        } else {
            order.side_     = OrderSide::Sell;
            order.quantity_ = notional;
        }
        order.price_ = 0.0;
        orders.push(spp::move(order));

        current_direction_ = signal.direction_;
        days_held_ = 0;

        return orders;
    }

    void on_fill(const FillEvent& fill) override {
        f64 signed_qty = fill.signed_quantity();
        f64 cost = fill.price_ * fill.quantity_ + fill.commission_;

        auto pos_opt = positions_.find(fill.symbol_);
        if (pos_opt.ok()) {
            Position& pos = **pos_opt;
            if (pos.quantity_ + signed_qty == 0.0) {
                positions_.remove(fill.symbol_);
            } else {
                f64 new_qty = pos.quantity_ + signed_qty;
                f64 new_cost_basis = pos.quantity_ * pos.entry_price_ + signed_qty * fill.price_;
                pos.quantity_ = new_qty;
                pos.entry_price_ = (new_qty != 0.0) ? new_cost_basis / new_qty : 0.0;
            }
        } else if (signed_qty != 0.0) {
            Position new_pos;
            new_pos.instrument_id_ = fill.symbol_;
            new_pos.quantity_      = signed_qty;
            new_pos.entry_price_   = fill.price_;
            new_pos.entry_date_    = fill.date_;
            positions_.add(spp::move(new_pos));
        }

        if (fill.side_ == OrderSide::Buy)
            cash_ -= cost;
        else
            cash_ += fill.price_ * fill.quantity_ - fill.commission_;
    }

    SPP_RECORD(Momentum, SPP_FIELD(lookback_), SPP_FIELD(holding_period_),
               SPP_FIELD(min_return_pct_));
};

} // namespace spp::quant

// =========================================================================
// SPP reflection records for strategy types only.
// Event type records are in core/types.h.
// SignalDirection and OrderSide reflection records are in core/types.h.
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::Strategy, "Strategy", SPP_FIELD(name_));

SPP_NAMED_RECORD(::spp::quant::MACrossover, "MACrossover",
                 SPP_FIELD(fast_window_), SPP_FIELD(slow_window_));

SPP_NAMED_RECORD(::spp::quant::MeanReversion, "MeanReversion",
                 SPP_FIELD(lookback_), SPP_FIELD(entry_z_score_),
                 SPP_FIELD(exit_z_score_));

SPP_NAMED_RECORD(::spp::quant::Momentum, "Momentum",
                 SPP_FIELD(lookback_), SPP_FIELD(holding_period_),
                 SPP_FIELD(min_return_pct_));
