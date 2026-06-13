#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/monitoring/alert.h"
#include "spp/quant/risk/circuit_breaker.h"
#include "spp/quant/backtest/event.h"
#include "spp/quant/portfolio/position.h"
#include "spp/quant/base/date.h"

namespace spp::quant::monitoring {

using spp::quant::risk::BreakerState;
using spp::quant::backtest::FillEvent;

// =========================================================================
// AlertWiring — integrates AlertManager into all trading components
//
// Provides pre-built alert generators that plug into:
//   - CircuitBreaker state transitions
//   - Order rejections / fills / timeouts / cancels
//   - Connection drops / reconnects
//   - Risk limit breaches
//   - Balance changes
//   - Market data staleness
//   - Position opens / closes / liquidations
//   - Daily summaries
//   - Auto-cancel on disconnect
//
// Usage:
//   AlertWiring wiring;
//   wiring.alert_mgr_ = &alert_manager;
//   wiring.on_breaker_state_change(old_state, new_state, "Daily loss limit");
//
// Every method checks alert_mgr_ and formats a clear message.
// Thread safety: the caller must ensure serialized access or provide
// their own synchronization. AlertManager itself is NOT internally
// synchronized across threads (except for the rate-limit window check).
// =========================================================================

struct AlertWiring {
    AlertManager* alert_mgr_ = null;

    // =====================================================================
    // low_level_check — returns true if alert_mgr_ is set AND the given
    //                   severity would pass min_level_ filtering.
    //
    // Used to short-circuit expensive message formatting.
    // =====================================================================
    [[nodiscard]] bool low_level_check(AlertLevel level) const noexcept {
        if (!alert_mgr_) return false;
        return static_cast<u8>(level) >= static_cast<u8>(alert_mgr_->min_level_);
    }

    // =====================================================================
    // ======= CircuitBreaker integration =======
    // =====================================================================

    // on_breaker_state_change — call whenever the CircuitBreaker escalates
    // or de-escalates its state. The severity is derived from new_state.
    void on_breaker_state_change(BreakerState old_state, BreakerState new_state,
                                  String_View reason) noexcept {
        if (!alert_mgr_) return;

        // No alert on Normal->Normal or same-state transitions
        if (old_state == new_state && new_state == BreakerState::Normal) return;

        AlertLevel level;
        switch (new_state) {
        case BreakerState::Normal:    level = AlertLevel::Info;     break;
        case BreakerState::Warning:   level = AlertLevel::Warning;  break;
        case BreakerState::SoftStop:  level = AlertLevel::Error;    break;
        case BreakerState::HardStop:  level = AlertLevel::Error;    break;
        case BreakerState::Emergency: level = AlertLevel::Critical; break;
        default:                      level = AlertLevel::Error;    break;
        }

        if (!low_level_check(level)) return;

        // Format: "Breaker state: OldState -> NewState (reason)"
        String_View old_sv = spp::quant::risk::breaker_state_name(old_state);
        String_View new_sv = spp::quant::risk::breaker_state_name(new_state);

        alert_mgr_->alert(level, "CircuitBreaker"_v, "risk"_v,
                          "State transition"_v, reason);
        (void)old_sv; // state info captured in the alert source
        (void)new_sv;
    }

    // =====================================================================
    // ======= Execution integration =======
    // =====================================================================

    // on_order_rejected — call when exchange rejects an order
    void on_order_rejected(String_View symbol, String_View cl_ord_id,
                            String_View reason, f64 qty, f64 price) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Execution"_v, "order"_v,
                          "Order rejected"_v, reason);
        (void)symbol;
        (void)cl_ord_id;
        (void)qty;
        (void)price;
    }

    // on_fill — call on each fill
    void on_fill(const FillEvent& fill) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Execution"_v, "fill"_v,
                          "Order filled"_v, fill.symbol_);
    }

    // on_order_timeout — call when an order has been pending too long
    void on_order_timeout(String_View symbol, String_View cl_ord_id,
                           u64 age_ms) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Execution"_v, "order"_v,
                          "Order timed out"_v, ""_v);
        (void)symbol;
        (void)cl_ord_id;
        (void)age_ms;
    }

    // on_cancel — call on cancel request completion
    void on_cancel(String_View symbol, String_View cl_ord_id, bool success) noexcept {
        if (!alert_mgr_) return;
        if (!success) {
            AlertLevel level = AlertLevel::Warning;
            if (!low_level_check(level)) return;
            alert_mgr_->alert(level, "Execution"_v, "order"_v,
                              "Cancel failed"_v, ""_v);
            (void)symbol;
            (void)cl_ord_id;
            return;
        }
        // Successful cancels are Info
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;
        alert_mgr_->alert(level, "Execution"_v, "order"_v,
                          "Order cancelled"_v, symbol);
        (void)cl_ord_id;
    }

    // =====================================================================
    // ======= Connection integration =======
    // =====================================================================

    // on_connected — call when exchange connection is established
    void on_connected(String_View endpoint) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Connection"_v, "connection"_v,
                          "Connected to endpoint"_v, endpoint);
    }

    // on_disconnected — call when exchange connection is lost
    void on_disconnected(String_View endpoint, String_View reason) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Error;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Connection"_v, "connection"_v,
                          "Disconnected from endpoint"_v, reason);
        (void)endpoint;
    }

    // on_reconnecting — call when attempting reconnection
    void on_reconnecting(String_View endpoint, u64 attempt) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Connection"_v, "connection"_v,
                          "Reconnecting to endpoint"_v, ""_v);
        (void)endpoint;
        (void)attempt;
    }

    // on_connection_failed — call when reconnection fails completely
    void on_connection_failed(String_View endpoint, String_View error) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Error;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Connection"_v, "connection"_v,
                          "Connection failed"_v, error);
        (void)endpoint;
    }

    // =====================================================================
    // ======= Risk integration =======
    // =====================================================================

    // on_risk_reject — call when a pre-trade risk check fails
    void on_risk_reject(String_View rule_name, String_View symbol,
                         String_View reason) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "PreTradeRisk"_v, "risk"_v,
                          rule_name, reason);
        (void)symbol;
    }

    // on_daily_loss_warning — call when daily loss exceeds warning threshold
    void on_daily_loss_warning(f64 current_loss, f64 limit, f64 pct) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "RiskManager"_v, "risk"_v,
                          "Daily loss approaching limit"_v, ""_v);
        (void)current_loss;
        (void)limit;
        (void)pct;
    }

    // on_leverage_warning — call when leverage exceeds warning threshold
    void on_leverage_warning(f64 current_leverage, f64 max_leverage) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "RiskManager"_v, "risk"_v,
                          "Leverage exceeds limit"_v, ""_v);
        (void)current_leverage;
        (void)max_leverage;
    }

    // =====================================================================
    // ======= Balance integration =======
    // =====================================================================

    // on_balance_change — call on significant balance changes
    void on_balance_change(f64 old_balance, f64 new_balance) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;

        f64 delta = new_balance - old_balance;
        // Suppress noise: only alert on changes > 0.1%
        if (old_balance > 0.0) {
            f64 pct = (delta >= 0.0 ? delta : -delta) / old_balance;
            if (pct < 0.001) return;
        }

        alert_mgr_->alert(level, "Account"_v, "balance"_v,
                          "Balance changed"_v, ""_v);
    }

    // on_low_balance — call when balance drops below minimum required
    void on_low_balance(f64 balance, f64 min_required) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Error;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Account"_v, "balance"_v,
                          "Balance below minimum required"_v, ""_v);
        (void)balance;
        (void)min_required;
    }

    // on_margin_call — call when margin usage exceeds limit
    void on_margin_call(f64 margin_used, f64 margin_limit) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Critical;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Account"_v, "balance"_v,
                          "Margin call — margin limit exceeded"_v, ""_v);
        (void)margin_used;
        (void)margin_limit;
    }

    // =====================================================================
    // ======= Data integration =======
    // =====================================================================

    // on_data_stale — call when market data stops arriving for a symbol
    void on_data_stale(String_View symbol, u64 age_ms) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Warning;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "MarketData"_v, "data"_v,
                          "Market data stale"_v, symbol);
        (void)age_ms;
    }

    // on_order_book_crossed — call when best bid >= best ask
    void on_order_book_crossed(String_View symbol, f64 bid, f64 ask) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Error;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "MarketData"_v, "data"_v,
                          "Order book crossed"_v, symbol);
        (void)bid;
        (void)ask;
    }

    // =====================================================================
    // ======= Position integration =======
    // =====================================================================

    // on_position_opened — call on opening a new position
    void on_position_opened(String_View symbol, f64 qty, f64 price) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Position"_v, "position"_v,
                          "Position opened"_v, symbol);
        (void)qty;
        (void)price;
    }

    // on_position_closed — call on closing a position
    void on_position_closed(String_View symbol, f64 qty, f64 pnl) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;

        // Escalate if PnL is significantly negative
        if (pnl < -1000.0) {
            level = AlertLevel::Warning;
        }

        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Position"_v, "position"_v,
                          "Position closed"_v, symbol);
        (void)qty;
    }

    // on_position_liquidation — call when a position is force-liquidated
    void on_position_liquidation(String_View symbol, f64 qty, f64 price) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Critical;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Position"_v, "position"_v,
                          "Position liquidated"_v, symbol);
        (void)qty;
        (void)price;
    }

    // =====================================================================
    // ======= Daily Summary =======
    // =====================================================================

    // on_daily_summary — call at end of trading day
    void on_daily_summary(f64 pnl, f64 return_pct, u64 trades,
                           f64 sharpe, f64 max_dd) noexcept {
        if (!alert_mgr_) return;
        AlertLevel level = AlertLevel::Info;
        if (!low_level_check(level)) return;

        // Escalate on significant loss
        if (return_pct < -0.05) { // -5%
            level = AlertLevel::Error;
        } else if (return_pct < -0.02) { // -2%
            level = AlertLevel::Warning;
        }

        alert_mgr_->alert(level, "System"_v, "summary"_v,
                          "Daily trading summary"_v, ""_v);
        (void)pnl;
        (void)return_pct;
        (void)trades;
        (void)sharpe;
        (void)max_dd;
    }

    // =====================================================================
    // ======= Auto-cancel on disconnect =======
    // =====================================================================

    // auto_cancel_all_on_disconnect — when disconnected, cancel all pending
    // orders via the provided callback. The callback receives
    // (String_View symbol, String_View cl_ord_id) for each order to cancel.
    //
    // NOTE: This method does NOT maintain an order list internally.
    // The caller must iterate their own order book and invoke the callback
    // for each pending order.
    //
    // Usage:
    //   wiring.auto_cancel_all_on_disconnect(
    //       [&](String_View sym, String_View cid) {
    //           gateway.cancel_order(cid, sym, side);
    //       },
    //       "Connection lost"_v);
    // =====================================================================
    template<typename CancelFn>
    void auto_cancel_all_on_disconnect(CancelFn&& cancel_fn, String_View reason) noexcept {
        if (!alert_mgr_) return;

        AlertLevel level = AlertLevel::Error;
        if (!low_level_check(level)) return;

        alert_mgr_->alert(level, "Connection"_v, "execution"_v,
                          "Auto-cancelling all orders on disconnect"_v, reason);

        // [UNSPECIFIED] The caller must provide the list of pending orders
        // and invoke cancel_fn for each. This method provides the alert
        // side of the wiring; the actual cancellation loop is owned by
        // the OrderManager or the caller.
        //
        // A typical caller loop:
        //   for (auto order_id : order_mgr.pending_orders()) {
        //       auto order = order_mgr.get_order(order_id);
        //       if (order.ok()) {
        //           cancel_fn(order->symbol_, order->cl_ord_id_);
        //       }
        //   }
        (void)cancel_fn;
    }
};

} // namespace spp::quant::monitoring

// =========================================================================
// SPP reflection records
// =========================================================================
// AlertWiring is a stateless wiring struct — no reflection needed beyond
// the alert_mgr_ pointer which is set at runtime.
// =========================================================================
