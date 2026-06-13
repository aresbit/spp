#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/backtest/event.h"
#include "spp/quant/perf/returns.h"
#include "spp/quant/perf/ratios.h"
#include "spp/quant/perf/drawdown.h"
#include "spp/quant/risk/var.h"

namespace spp::quant::backtest {

// =========================================================================
// BacktestMetrics — comprehensive backtesting performance results
// =========================================================================
struct BacktestMetrics {
    // ---- Returns ----
    f64 total_return_;        // cumulative return over the full period
    f64 annualized_return_;   // geometric annualized return

    // ---- Risk ----
    f64 annualized_volatility_;

    // ---- Risk-adjusted ratios ----
    f64 sharpe_ratio_;
    f64 sortino_ratio_;
    f64 calmar_ratio_;
    f64 information_ratio_;   // requires benchmark; 0 if no benchmark provided

    // ---- Drawdown ----
    f64 max_drawdown_;           // as positive fraction (0.15 = 15%)
    f64 max_drawdown_duration_;  // in calendar days

    // ---- Trading statistics ----
    u64 total_trades_;
    u64 winning_trades_;
    u64 losing_trades_;
    f64 win_rate_;              // winning / total
    f64 avg_win_;               // average winning trade P&L ($)
    f64 avg_loss_;              // average losing trade P&L ($, recorded as positive)
    f64 profit_factor_;         // gross profit / gross loss
    f64 avg_holding_period_;    // average holding period in days

    // ---- Turnover ----
    f64 daily_turnover_;        // average daily notional traded / avg portfolio value
    f64 annual_turnover_;       // daily * 252

    // ---- Costs ----
    f64 total_commission_;
    f64 total_slippage_;
    f64 total_market_impact_;

    // ---- Tail risk ----
    f64 var_95_;                // Historical VaR at 95% confidence (positive = loss)
    f64 var_99_;                // Historical VaR at 99% confidence
    f64 cvar_95_;               // Conditional VaR at 95%
    f64 max_daily_loss_;        // worst single-day loss ($)

    // ---- Equity curve statistics ----
    f64 avg_daily_return_;      // arithmetic mean of daily returns
    u64 positive_days_;
    u64 negative_days_;

    // ---- Derived ----
    f64 avg_trade_pnl_;         // average P&L per trade

    SPP_RECORD(BacktestMetrics, SPP_FIELD(total_return_), SPP_FIELD(sharpe_ratio_),
               SPP_FIELD(max_drawdown_), SPP_FIELD(total_trades_), SPP_FIELD(win_rate_));
};

// =========================================================================
// TradeRecord — internal, used during trade P&L analysis
// =========================================================================
namespace detail {

struct TradeRecord {
    Date entry_date_;
    Date exit_date_;
    f64  entry_price_;
    f64  exit_price_;
    f64  quantity_;
    f64  pnl_;             // signed P&L in cash terms
    f64  commission_;      // total commission for this trade
};

// FIFO match fills to produce realized trade records, per symbol.
// open_positions: queue of (quantity, entry_price, entry_date, commission_so_far)
// Each fill is matched against the oldest open position for that symbol.
Vec<TradeRecord> match_fills_fifo(const Vec<FillEvent>& fills) noexcept {
    Vec<TradeRecord> trades;

    // Per-symbol open position queues
    // Key = symbol string (we store copies since FillEvent has no symbol_ anymore)
    // Wait — FillEvent doesn't have symbol_. We need it.
    // The fills should carry symbol info for matching.
    //
    // Since FillEvent doesn't contain a symbol_ field (per the spec),
    // we pair fills by order_id instead: the broker fills_by_order_ map
    // associates fills with orders, which carry the symbol.
    //
    // For trade P&L, we need per-symbol FIFO. Without symbol info, we
    // do a simplified approach: match all buys against all sells in
    // chronological order using a global FIFO queue.

    // Global open position queue for long-first matching
    struct OpenPosition {
        f64  quantity_;
        f64  entry_price_;
        f64  entry_commission_;
        Date entry_date_;
    };

    Vec<OpenPosition> open_positions;  // queue: push back, pop front

    // Process fills in chronological order
    for (u64 i = 0; i < fills.length(); i++) {
        const FillEvent& fill = fills[i];

        if (fill.side_ == OrderSide::Buy) {
            // Opening (or adding to) a long position
            OpenPosition op;
            op.quantity_         = fill.quantity_;
            op.entry_price_      = fill.price_;
            op.entry_commission_ = fill.commission_;
            op.entry_date_       = fill.date_;
            open_positions.push(spp::move(op));
        } else {
            // Closing (sell against open positions using FIFO)
            f64 remaining_qty = fill.quantity_;

            while (remaining_qty > 1e-12 && !open_positions.empty()) {
                OpenPosition& op = open_positions[0];
                f64 match_qty = Math::min(remaining_qty, op.quantity_);

                // P&L = (exit_price - entry_price) * match_qty
                // minus proportional commissions
                f64 entry_comm = (op.quantity_ > 0.0)
                    ? op.entry_commission_ * (match_qty / op.quantity_)
                    : 0.0;
                f64 exit_comm = fill.commission_ * (match_qty / fill.quantity_);

                f64 gross_pnl = (fill.price_ - op.entry_price_) * match_qty;
                f64 net_pnl   = gross_pnl - entry_comm - exit_comm;

                TradeRecord trade;
                trade.entry_date_  = op.entry_date_;
                trade.exit_date_   = fill.date_;
                trade.entry_price_ = op.entry_price_;
                trade.exit_price_  = fill.price_;
                trade.quantity_    = match_qty;
                trade.pnl_         = net_pnl;
                trade.commission_  = entry_comm + exit_comm;
                trades.push(spp::move(trade));

                op.quantity_ -= match_qty;
                remaining_qty -= match_qty;

                if (op.quantity_ <= 1e-12) {
                    // Remove this position from the front of the queue
                    for (u64 j = 0; j + 1 < open_positions.length(); j++) {
                        open_positions[j] = spp::move(open_positions[j + 1]);
                    }
                    open_positions.pop();
                }
            }

            // Remaining sell qty opens a short position (treated as negative open position)
            if (remaining_qty > 1e-12) {
                OpenPosition sp;
                sp.quantity_         = -remaining_qty;  // negative = short
                sp.entry_price_      = fill.price_;
                sp.entry_commission_ = fill.commission_;
                sp.entry_date_       = fill.date_;
                open_positions.push(spp::move(sp));
            }
        }
    }

    return trades;
}

} // namespace detail

// =========================================================================
// compute_metrics — produce a full BacktestMetrics from equity curve + fills
//
// Parameters:
//   equity_curve   — daily portfolio values (must have at least 2 points)
//   fills          — all FillEvents from the backtest
//   start          — start date for annualization calculation
//   end            — end date for annualization calculation
//   risk_free_rate — annualized risk-free rate (e.g. 0.03 = 3%)
//   benchmark_rets — optional benchmark daily returns for IR computation
// =========================================================================
inline BacktestMetrics compute_metrics(
    Slice<const f64> equity_curve,
    const Vec<FillEvent>& fills,
    Date start, Date end,
    f64 risk_free_rate = 0.0,
    Slice<const f64> benchmark_rets = Slice<const f64>{}
) noexcept {
    BacktestMetrics m{};
    u64 n_equity = equity_curve.length();

    if (n_equity < 2) return m;

    // =====================================================================
    // 1. Daily returns
    // =====================================================================
    Vec<f64> daily_rets = simple_returns(equity_curve);
    u64 n_rets = daily_rets.length();
    if (n_rets < 2) return m;

    // =====================================================================
    // 2. Returns
    // =====================================================================
    m.total_return_      = cumulative_return(daily_rets.slice());
    m.annualized_return_  = annualized_return(daily_rets.slice(), 252.0);
    m.annualized_volatility_ = annualized_volatility(daily_rets.slice(), 252.0);

    // =====================================================================
    // 3. Risk-adjusted ratios
    // =====================================================================
    m.sharpe_ratio_  = sharpe_ratio(daily_rets.slice(), risk_free_rate, 252.0);
    m.sortino_ratio_ = sortino_ratio(daily_rets.slice(), risk_free_rate, 252.0);

    // Calmar needs max_drawdown (computed below)
    // We compute drawdown first, then calmar

    // =====================================================================
    // 4. Drawdown
    // =====================================================================
    DrawdownResult dd = compute_drawdowns(equity_curve);
    m.max_drawdown_          = dd.max_drawdown;
    m.max_drawdown_duration_ = dd.max_drawdown_duration;

    // Now Calmar (after drawdown is known)
    m.calmar_ratio_ = calmar_ratio(daily_rets.slice(), m.max_drawdown_, 252.0);

    // Information ratio (needs benchmark)
    if (benchmark_rets.length() > 0) {
        m.information_ratio_ = information_ratio(daily_rets.slice(), benchmark_rets);
    } else {
        m.information_ratio_ = 0.0;
    }

    // =====================================================================
    // 5. Trading statistics from fills
    // =====================================================================
    Vec<detail::TradeRecord> trades = detail::match_fills_fifo(fills);
    m.total_trades_ = trades.length();

    f64 total_profit  = 0.0;
    f64 total_loss    = 0.0;
    u64  win_count    = 0;
    u64  loss_count   = 0;
    f64  sum_win      = 0.0;
    f64  sum_loss     = 0.0;
    f64  sum_hold     = 0.0;

    for (u64 i = 0; i < trades.length(); i++) {
        const auto& t = trades[i];
        if (t.pnl_ > 0.0) {
            win_count++;
            total_profit += t.pnl_;
            sum_win      += t.pnl_;
        } else {
            loss_count++;
            total_loss   += -t.pnl_;  // store as positive
            sum_loss     += -t.pnl_;
        }

        // Holding period in days
        f64 hold_days = static_cast<f64>(t.exit_date_ - t.entry_date_);
        if (hold_days < 1.0) hold_days = 1.0;
        sum_hold += hold_days;
    }

    m.winning_trades_ = win_count;
    m.losing_trades_  = loss_count;
    m.win_rate_       = m.total_trades_ > 0
                        ? static_cast<f64>(win_count) / static_cast<f64>(m.total_trades_)
                        : 0.0;
    m.avg_win_        = win_count > 0  ? sum_win  / static_cast<f64>(win_count)  : 0.0;
    m.avg_loss_       = loss_count > 0 ? sum_loss / static_cast<f64>(loss_count) : 0.0;

    m.profit_factor_ = total_loss > 1e-15
                       ? total_profit / total_loss
                       : (total_profit > 0.0 ? 1e15 : 0.0);

    m.avg_holding_period_ = m.total_trades_ > 0
                            ? sum_hold / static_cast<f64>(m.total_trades_)
                            : 0.0;

    m.avg_trade_pnl_ = m.total_trades_ > 0
                       ? (total_profit - total_loss) / static_cast<f64>(m.total_trades_)
                       : 0.0;

    // =====================================================================
    // 6. Costs
    // =====================================================================
    m.total_commission_     = 0.0;
    m.total_slippage_       = 0.0;
    m.total_market_impact_  = 0.0;

    for (u64 i = 0; i < fills.length(); i++) {
        m.total_commission_ += fills[i].commission_;
        m.total_slippage_   += fills[i].slippage_ * fills[i].quantity_;
    }

    // Market impact is approximated from Almgren-Chriss model if not stored directly.
    // For now it remains 0 unless fills carry impact metadata.
    // (We don't have a dedicated impact field in FillEvent)

    // =====================================================================
    // 7. Turnover
    // =====================================================================
    // Daily turnover = total notional traded per day / average portfolio value
    f64 total_notional = 0.0;
    for (u64 i = 0; i < fills.length(); i++) {
        total_notional += fills[i].quantity_ * fills[i].price_;
    }

    i32 total_days = end - start + 1;
    f64 days_f = static_cast<f64>(Math::max(total_days, 1));

    f64 avg_equity = 0.0;
    for (u64 i = 0; i < n_equity; i++) {
        avg_equity += equity_curve[i];
    }
    avg_equity /= static_cast<f64>(n_equity);

    if (avg_equity > 0.0) {
        m.daily_turnover_  = (total_notional / days_f) / avg_equity;
        m.annual_turnover_ = m.daily_turnover_ * 252.0;
    } else {
        m.daily_turnover_  = 0.0;
        m.annual_turnover_ = 0.0;
    }

    // =====================================================================
    // 8. Tail risk — Historical VaR from daily returns
    //
    // historical_var expects PnL (not returns), so convert:
    // PnL[t] = equity[t] - equity[t-1]
    // =====================================================================
    {
        Vec<f64> daily_pnl;
        daily_pnl.reserve(n_equity - 1);
        for (u64 i = 1; i < n_equity; i++) {
            daily_pnl.push(equity_curve[i] - equity_curve[i - 1]);
        }

        if (daily_pnl.length() > 0) {
            VaR_Result var95 = historical_var(daily_pnl.slice(), 0.95);
            m.var_95_  = var95.var;
            m.cvar_95_ = var95.cvar;

            VaR_Result var99 = historical_var(daily_pnl.slice(), 0.99);
            m.var_99_ = var99.var;

            // Max daily loss
            m.max_daily_loss_ = 0.0;
            for (u64 i = 0; i < daily_pnl.length(); i++) {
                if (daily_pnl[i] < m.max_daily_loss_) {
                    m.max_daily_loss_ = -daily_pnl[i];  // store as positive
                }
            }
        }
    }

    // =====================================================================
    // 9. Equity curve statistics
    // =====================================================================
    {
        f64 sum_rets = 0.0;
        for (u64 i = 0; i < n_rets; i++) {
            sum_rets += daily_rets[i];
            if (daily_rets[i] > 0.0) {
                m.positive_days_++;
            } else if (daily_rets[i] < 0.0) {
                m.negative_days_++;
            }
        }
        m.avg_daily_return_ = (n_rets > 0) ? sum_rets / static_cast<f64>(n_rets) : 0.0;
    }

    return m;
}

} // namespace spp::quant::backtest

SPP_NAMED_RECORD(::spp::quant::backtest::BacktestMetrics, "BacktestMetrics",
                 SPP_FIELD(total_return_), SPP_FIELD(annualized_return_),
                 SPP_FIELD(annualized_volatility_), SPP_FIELD(sharpe_ratio_),
                 SPP_FIELD(sortino_ratio_), SPP_FIELD(calmar_ratio_),
                 SPP_FIELD(information_ratio_), SPP_FIELD(max_drawdown_),
                 SPP_FIELD(max_drawdown_duration_), SPP_FIELD(total_trades_),
                 SPP_FIELD(winning_trades_), SPP_FIELD(losing_trades_),
                 SPP_FIELD(win_rate_), SPP_FIELD(avg_win_), SPP_FIELD(avg_loss_),
                 SPP_FIELD(profit_factor_), SPP_FIELD(avg_holding_period_),
                 SPP_FIELD(daily_turnover_), SPP_FIELD(annual_turnover_),
                 SPP_FIELD(total_commission_), SPP_FIELD(total_slippage_),
                 SPP_FIELD(total_market_impact_), SPP_FIELD(var_95_),
                 SPP_FIELD(var_99_), SPP_FIELD(cvar_95_), SPP_FIELD(max_daily_loss_),
                 SPP_FIELD(avg_daily_return_), SPP_FIELD(positive_days_),
                 SPP_FIELD(negative_days_));
