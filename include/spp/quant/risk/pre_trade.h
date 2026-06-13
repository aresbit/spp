#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

namespace spp::quant::risk {

// =========================================================================
// PreTradeResult — outcome of a pre-trade risk check
// =========================================================================
struct PreTradeResult {
    bool        approved_  = true;
    String_View reason_    = ""_v;
    String_View rule_name_ = ""_v;

    static PreTradeResult approve() noexcept {
        PreTradeResult r;
        r.approved_  = true;
        r.reason_    = ""_v;
        r.rule_name_ = ""_v;
        return r;
    }

    static PreTradeResult reject(String_View rule, String_View reason) noexcept {
        PreTradeResult r;
        r.approved_  = false;
        r.rule_name_ = rule;
        r.reason_    = reason;
        return r;
    }

    SPP_RECORD(PreTradeResult, SPP_FIELD(approved_), SPP_FIELD(reason_), SPP_FIELD(rule_name_));
};

// =========================================================================
// RiskLimit — a single named risk limit
// =========================================================================
struct RiskLimit {
    String_View name_;
    f64         limit_value_ = 0.0;
    bool        enabled_     = true;

    SPP_RECORD(RiskLimit, SPP_FIELD(name_), SPP_FIELD(limit_value_), SPP_FIELD(enabled_));
};

// =========================================================================
// PreTradeRisk — validates orders BEFORE they reach the exchange
//
// All checks return a PreTradeResult (approved_ = true means pass,
// approved_ = false means rejection with a reason and rule name).
//
// Usage:
//   PreTradeRisk risk;
//   risk.max_order_size_.limit_value_ = 1000.0;
//   risk.max_order_value_.limit_value_ = 500'000.0;
//   ...
//   auto result = risk.check_order(qty, price, symbol, pos, pos_px, nav, gross, net);
//   if (!result.approved_) { log_rejection(result); return; }
// =========================================================================
struct PreTradeRisk {
    // ---- Order-level limits ----
    RiskLimit max_order_size_{
        "max_order_size"_v, 0.0, true};     // maximum single order quantity
    RiskLimit max_order_value_{
        "max_order_value"_v, 0.0, true};    // max order notional (price * qty)
    RiskLimit min_order_size_{
        "min_order_size"_v, 0.0, true};     // minimum order quantity
    RiskLimit max_price_deviation_{
        "max_price_deviation"_v, 0.05, true}; // max % deviation from last price

    // ---- Position-level limits ----
    RiskLimit max_position_size_{
        "max_position_size"_v, 0.0, true};    // max position qty per instrument
    RiskLimit max_position_value_{
        "max_position_value"_v, 0.0, true};   // max position notional per instrument
    RiskLimit max_gross_exposure_{
        "max_gross_exposure"_v, 0.0, true};   // max total gross exposure
    RiskLimit max_net_exposure_{
        "max_net_exposure"_v, 0.0, true};     // max net directional exposure
    RiskLimit max_leverage_{
        "max_leverage"_v, 0.0, true};         // max leverage ratio

    // ---- Daily limits (reset at start of day) ----
    RiskLimit max_daily_loss_{
        "max_daily_loss"_v, 0.0, true};         // max daily PnL loss before stop
    RiskLimit max_daily_trades_{
        "max_daily_trades"_v, 0.0, true};       // max number of trades per day
    RiskLimit max_daily_orders_{
        "max_daily_orders"_v, 0.0, true};       // max order submissions per day
    RiskLimit max_daily_cancel_rate_{
        "max_daily_cancel_rate"_v, 0.0, true};  // max cancellation rate

    // ---- Tracking state (reset daily) ----
    f64 daily_pnl_      = 0.0;
    u64 daily_trades_   = 0;
    u64 daily_orders_   = 0;
    u64 daily_cancels_  = 0;
    f64 peak_daily_pnl_ = 0.0;

    // =====================================================================
    // check_order — validate a new order against all enabled limits
    //
    // Parameters:
    //   order_qty             — proposed order quantity (positive = buy, negative = sell)
    //   order_price           — proposed order price
    //   symbol                — instrument identifier (for diagnostic messages)
    //   current_position_qty  — existing position quantity
    //   last_market_price     — last traded price (for deviation check)
    //   portfolio_value       — current total portfolio NAV
    //   gross_exposure        — current total gross exposure
    //   net_exposure          — current total net exposure
    // =====================================================================
    PreTradeResult check_order(
        f64 order_qty,
        f64 order_price,
        String_View symbol,
        f64 current_position_qty,
        f64 last_market_price,
        f64 portfolio_value,
        f64 gross_exposure,
        f64 net_exposure) const noexcept
    {
        // ---- Utility: check a single limit with a numeric check ----
        auto check_limit = [](const RiskLimit& limit, bool condition,
                               String_View detail) -> Opt<PreTradeResult> {
            if (!limit.enabled_) return {};
            if (condition) {
                return Opt<PreTradeResult>{PreTradeResult::reject(limit.name_, detail)};
            }
            return {};
        };

        f64 order_notional = order_qty * order_price;
        // Direction multiplier: +1 for buy (long), -1 for sell (short)
        // For min_order_size we use absolute quantity
        f64 abs_qty = (order_qty >= 0.0) ? order_qty : -order_qty;
        f64 abs_notional = (order_notional >= 0.0) ? order_notional : -order_notional;

        // 1. Max order size (absolute quantity)
        if (max_order_size_.enabled_ && max_order_size_.limit_value_ > 0.0) {
            if (abs_qty > max_order_size_.limit_value_) {
                return PreTradeResult::reject(
                    max_order_size_.name_,
                    ""_v  // caller fills via context
                );
            }
        }

        // 2. Min order size (absolute quantity)
        if (min_order_size_.enabled_ && min_order_size_.limit_value_ > 0.0) {
            if (abs_qty < min_order_size_.limit_value_) {
                return PreTradeResult::reject(
                    min_order_size_.name_,
                    ""_v
                );
            }
        }

        // 3. Max order value (absolute notional)
        if (max_order_value_.enabled_ && max_order_value_.limit_value_ > 0.0) {
            if (abs_notional > max_order_value_.limit_value_) {
                return PreTradeResult::reject(
                    max_order_value_.name_,
                    ""_v
                );
            }
        }

        // 4. Max price deviation (fat-finger protection)
        // Compare order_price to last_market_price
        if (max_price_deviation_.enabled_ && max_price_deviation_.limit_value_ > 0.0) {
            if (last_market_price > 0.0 && order_price > 0.0) {
                f64 deviation = Math::abs(order_price - last_market_price) / last_market_price;
                if (deviation > max_price_deviation_.limit_value_) {
                    return PreTradeResult::reject(
                        max_price_deviation_.name_,
                        ""_v
                    );
                }
            }
        }

        // 5. Max position size (after the trade)
        if (max_position_size_.enabled_ && max_position_size_.limit_value_ > 0.0) {
            f64 new_pos_qty = current_position_qty + order_qty;
            f64 new_abs_qty = (new_pos_qty >= 0.0) ? new_pos_qty : -new_pos_qty;
            if (new_abs_qty > max_position_size_.limit_value_) {
                return PreTradeResult::reject(
                    max_position_size_.name_,
                    ""_v
                );
            }
        }

        // 6. Max position value (after the trade)
        if (max_position_value_.enabled_ && max_position_value_.limit_value_ > 0.0) {
            f64 new_pos_val = (current_position_qty + order_qty) * order_price;
            f64 new_abs_val = (new_pos_val >= 0.0) ? new_pos_val : -new_pos_val;
            if (new_abs_val > max_position_value_.limit_value_) {
                return PreTradeResult::reject(
                    max_position_value_.name_,
                    ""_v
                );
            }
        }

        // 7. Max gross exposure (after the trade)
        if (max_gross_exposure_.enabled_ && max_gross_exposure_.limit_value_ > 0.0) {
            // New gross = existing gross + |new order notional|
            // But need to account for closing a position — the gross only adds the
            // incremental exposure. A more conservative approach: add the full
            // absolute notional of the new order, since we are entering the order.
            f64 new_gross = gross_exposure + abs_notional;
            if (new_gross > max_gross_exposure_.limit_value_) {
                return PreTradeResult::reject(
                    max_gross_exposure_.name_,
                    ""_v
                );
            }
        }

        // 8. Max net exposure (after the trade)
        if (max_net_exposure_.enabled_ && max_net_exposure_.limit_value_ > 0.0) {
            f64 new_net = net_exposure + order_notional;
            f64 new_abs_net = (new_net >= 0.0) ? new_net : -new_net;
            if (new_abs_net > max_net_exposure_.limit_value_) {
                return PreTradeResult::reject(
                    max_net_exposure_.name_,
                    ""_v
                );
            }
        }

        // 9. Max leverage
        if (max_leverage_.enabled_ && max_leverage_.limit_value_ > 0.0) {
            if (portfolio_value > 0.0) {
                f64 new_gross = gross_exposure + abs_notional;
                f64 leverage = new_gross / portfolio_value;
                if (leverage > max_leverage_.limit_value_) {
                    return PreTradeResult::reject(
                        max_leverage_.name_,
                        ""_v
                    );
                }
            }
        }

        // 10. Max daily loss
        if (max_daily_loss_.enabled_ && max_daily_loss_.limit_value_ > 0.0) {
            if (daily_pnl_ <= -max_daily_loss_.limit_value_) {
                return PreTradeResult::reject(
                    max_daily_loss_.name_,
                    ""_v
                );
            }
        }

        // 11. Max daily trades
        if (max_daily_trades_.enabled_ && max_daily_trades_.limit_value_ > 0.0) {
            if (daily_trades_ >= static_cast<u64>(max_daily_trades_.limit_value_)) {
                return PreTradeResult::reject(
                    max_daily_trades_.name_,
                    ""_v
                );
            }
        }

        // 12. Max daily orders
        if (max_daily_orders_.enabled_ && max_daily_orders_.limit_value_ > 0.0) {
            if (daily_orders_ >= static_cast<u64>(max_daily_orders_.limit_value_)) {
                return PreTradeResult::reject(
                    max_daily_orders_.name_,
                    ""_v
                );
            }
        }

        // 13. Max daily cancel rate
        if (max_daily_cancel_rate_.enabled_ && max_daily_cancel_rate_.limit_value_ > 0.0) {
            if (daily_orders_ > 0) {
                f64 cancel_rate = static_cast<f64>(daily_cancels_) /
                                  static_cast<f64>(daily_orders_);
                if (cancel_rate > max_daily_cancel_rate_.limit_value_) {
                    return PreTradeResult::reject(
                        max_daily_cancel_rate_.name_,
                        ""_v
                    );
                }
            }
        }

        // All checks passed
        return PreTradeResult::approve();
    }

    // =====================================================================
    // check_cancel — validate a cancel request
    //
    // Currently checks: cancel rate limit
    // =====================================================================
    PreTradeResult check_cancel(String_View /*symbol*/) const noexcept {
        if (max_daily_cancel_rate_.enabled_ && max_daily_cancel_rate_.limit_value_ > 0.0) {
            // Use daily_orders_ + 1 to account for the cancel order itself being
            // sent through the system. If cancels exceed the rate even before
            // this cancel, reject it.
            u64 effective_orders = daily_orders_ + 1;
            if (effective_orders > 0) {
                f64 projected_rate = static_cast<f64>(daily_cancels_ + 1) /
                                     static_cast<f64>(effective_orders);
                if (projected_rate > max_daily_cancel_rate_.limit_value_) {
                    return PreTradeResult::reject(
                        max_daily_cancel_rate_.name_,
                        ""_v
                    );
                }
            }
        }

        if (max_daily_orders_.enabled_ && max_daily_orders_.limit_value_ > 0.0) {
            if (daily_orders_ >= static_cast<u64>(max_daily_orders_.limit_value_)) {
                return PreTradeResult::reject(
                    max_daily_orders_.name_,
                    ""_v
                );
            }
        }

        return PreTradeResult::approve();
    }

    // =====================================================================
    // record_fill — record a fill for daily PnL and trade tracking
    // =====================================================================
    void record_fill(f64 pnl, f64 /*qty*/) noexcept {
        daily_pnl_    += pnl;
        daily_trades_ += 1;
        if (daily_pnl_ > peak_daily_pnl_) {
            peak_daily_pnl_ = daily_pnl_;
        }
    }

    // =====================================================================
    // record_order — increment daily order counter
    // =====================================================================
    void record_order() noexcept {
        daily_orders_ += 1;
    }

    // =====================================================================
    // record_cancel — increment daily cancel counter
    // =====================================================================
    void record_cancel() noexcept {
        daily_cancels_ += 1;
    }

    // =====================================================================
    // reset_daily — reset all daily counters (call at start of trading day)
    // =====================================================================
    void reset_daily() noexcept {
        daily_pnl_       = 0.0;
        daily_trades_    = 0;
        daily_orders_    = 0;
        daily_cancels_   = 0;
        peak_daily_pnl_  = 0.0;
    }

    // =====================================================================
    // is_daily_loss_breached — true if daily loss exceeds the limit
    // =====================================================================
    [[nodiscard]] bool is_daily_loss_breached() const noexcept {
        if (!max_daily_loss_.enabled_) return false;
        return daily_pnl_ <= -max_daily_loss_.limit_value_;
    }

    SPP_RECORD(PreTradeRisk,
               SPP_FIELD(max_order_size_),
               SPP_FIELD(max_order_value_),
               SPP_FIELD(min_order_size_),
               SPP_FIELD(max_price_deviation_),
               SPP_FIELD(max_position_size_),
               SPP_FIELD(max_position_value_),
               SPP_FIELD(max_gross_exposure_),
               SPP_FIELD(max_net_exposure_),
               SPP_FIELD(max_leverage_),
               SPP_FIELD(max_daily_loss_),
               SPP_FIELD(max_daily_trades_),
               SPP_FIELD(max_daily_orders_),
               SPP_FIELD(max_daily_cancel_rate_),
               SPP_FIELD(daily_pnl_),
               SPP_FIELD(daily_trades_),
               SPP_FIELD(daily_orders_),
               SPP_FIELD(daily_cancels_),
               SPP_FIELD(peak_daily_pnl_));
};

// =========================================================================
// RiskProfiles — pre-built risk profiles for different trading styles
// =========================================================================
namespace RiskProfiles {

inline PreTradeRisk conservative() noexcept {
    PreTradeRisk r;
    r.max_order_size_.limit_value_       = 100.0;
    r.max_order_value_.limit_value_      = 100'000.0;
    r.min_order_size_.limit_value_       = 1.0;
    r.max_price_deviation_.limit_value_  = 0.02;   // 2% max deviation
    r.max_position_size_.limit_value_    = 500.0;
    r.max_position_value_.limit_value_   = 500'000.0;
    r.max_gross_exposure_.limit_value_   = 1'000'000.0;
    r.max_net_exposure_.limit_value_     = 500'000.0;
    r.max_leverage_.limit_value_         = 2.0;
    r.max_daily_loss_.limit_value_       = 50'000.0;
    r.max_daily_trades_.limit_value_     = 100.0;
    r.max_daily_orders_.limit_value_     = 500.0;
    r.max_daily_cancel_rate_.limit_value_ = 0.20;  // 20% max cancel rate
    return r;
}

inline PreTradeRisk moderate() noexcept {
    PreTradeRisk r;
    r.max_order_size_.limit_value_       = 1'000.0;
    r.max_order_value_.limit_value_      = 1'000'000.0;
    r.min_order_size_.limit_value_       = 1.0;
    r.max_price_deviation_.limit_value_  = 0.05;   // 5% max deviation
    r.max_position_size_.limit_value_    = 5'000.0;
    r.max_position_value_.limit_value_   = 5'000'000.0;
    r.max_gross_exposure_.limit_value_   = 10'000'000.0;
    r.max_net_exposure_.limit_value_     = 5'000'000.0;
    r.max_leverage_.limit_value_         = 4.0;
    r.max_daily_loss_.limit_value_       = 200'000.0;
    r.max_daily_trades_.limit_value_     = 500.0;
    r.max_daily_orders_.limit_value_     = 2'000.0;
    r.max_daily_cancel_rate_.limit_value_ = 0.30;
    return r;
}

inline PreTradeRisk aggressive() noexcept {
    PreTradeRisk r;
    r.max_order_size_.limit_value_       = 10'000.0;
    r.max_order_value_.limit_value_      = 10'000'000.0;
    r.min_order_size_.limit_value_       = 1.0;
    r.max_price_deviation_.limit_value_  = 0.10;   // 10% max deviation
    r.max_position_size_.limit_value_    = 50'000.0;
    r.max_position_value_.limit_value_   = 50'000'000.0;
    r.max_gross_exposure_.limit_value_   = 100'000'000.0;
    r.max_net_exposure_.limit_value_     = 50'000'000.0;
    r.max_leverage_.limit_value_         = 10.0;
    r.max_daily_loss_.limit_value_       = 1'000'000.0;
    r.max_daily_trades_.limit_value_     = 5'000.0;
    r.max_daily_orders_.limit_value_     = 20'000.0;
    r.max_daily_cancel_rate_.limit_value_ = 0.50;
    return r;
}

inline PreTradeRisk market_making() noexcept {
    PreTradeRisk r;
    // Market making: high turnover, low per-trade risk, tight deviations
    r.max_order_size_.limit_value_       = 500.0;
    r.max_order_value_.limit_value_      = 500'000.0;
    r.min_order_size_.limit_value_       = 1.0;
    r.max_price_deviation_.limit_value_  = 0.01;   // 1% tight deviation for MM
    r.max_position_size_.limit_value_    = 10'000.0;
    r.max_position_value_.limit_value_   = 10'000'000.0;
    r.max_gross_exposure_.limit_value_   = 20'000'000.0;
    r.max_net_exposure_.limit_value_     = 2'000'000.0;  // tight net exposure for MM
    r.max_leverage_.limit_value_         = 5.0;
    r.max_daily_loss_.limit_value_       = 100'000.0;
    r.max_daily_trades_.limit_value_     = 100'000.0;    // high trade volume
    r.max_daily_orders_.limit_value_     = 500'000.0;    // high order volume
    r.max_daily_cancel_rate_.limit_value_ = 0.90;        // high cancel rate is normal for MM
    return r;
}

}  // namespace RiskProfiles

}  // namespace spp::quant::risk
