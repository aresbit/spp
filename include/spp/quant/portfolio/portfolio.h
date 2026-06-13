#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/portfolio/position.h"
#include "spp/quant/data/timeseries.h"
#include "spp/quant/risk/var.h"

namespace spp::quant {

// =========================================================================
// Portfolio — aggregated set of positions with current prices
// =========================================================================
struct Portfolio {
    PositionBook          book_;
    Map<String_View, f64> market_prices_;  ///< instrument_id -> current price
    Date                  as_of_           = Date{};
    Currency_Code         base_currency_   = Currency_Code::USD;

    // -------------------------------------------------------------------
    // total_value — sum of (quantity * current_price) for all positions
    // -------------------------------------------------------------------
    [[nodiscard]] f64 total_value() const noexcept {
        f64 total = 0.0;
        for (u64 i = 0; i < book_.positions_.length(); i++) {
            const Position& pos = book_.positions_[i];
            f64 px = current_price(pos.instrument_id_);
            total += pos.quantity_ * px;
        }
        return total;
    }

    // -------------------------------------------------------------------
    // position_values — vector of individual position market values
    // -------------------------------------------------------------------
    [[nodiscard]] Vec<f64> position_values() const noexcept {
        u64 n = book_.positions_.length();
        Vec<f64> vals = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            const Position& pos = book_.positions_[i];
            vals[i] = pos.quantity_ * current_price(pos.instrument_id_);
        }
        return vals;
    }

    // -------------------------------------------------------------------
    // position_pnl — vector of unrealized PnL per position
    // -------------------------------------------------------------------
    [[nodiscard]] Vec<f64> position_pnl() const noexcept {
        u64 n = book_.positions_.length();
        Vec<f64> pnls = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            const Position& pos = book_.positions_[i];
            pnls[i] = pos.unrealized_pnl(current_price(pos.instrument_id_));
        }
        return pnls;
    }

    // -------------------------------------------------------------------
    // weights — position weights normalized to sum to 1
    // -------------------------------------------------------------------
    [[nodiscard]] Vec<f64> weights() const noexcept {
        u64 n = book_.positions_.length();
        Vec<f64> w = Vec<f64>::make(n);
        if (n == 0) return w;

        f64 total = total_value();
        if (total == 0.0) {
            // Equal weight fallback
            for (u64 i = 0; i < n; i++) {
                w[i] = 1.0 / static_cast<f64>(n);
            }
            return w;
        }

        for (u64 i = 0; i < n; i++) {
            const Position& pos = book_.positions_[i];
            w[i] = pos.quantity_ * current_price(pos.instrument_id_) / total;
        }
        return w;
    }

    // -------------------------------------------------------------------
    // returns — portfolio return series from price history
    //
    // portfolio_ts should be a time series of total portfolio values.
    // Returns simple return series: r_t = (V_t - V_{t-1}) / V_{t-1}
    // -------------------------------------------------------------------
    [[nodiscard]] Vec<f64> returns(const TimeSeries<f64>& portfolio_ts) const noexcept {
        auto dates  = portfolio_ts.dates();
        auto values = portfolio_ts.values();
        u64 n = values.length();
        if (n < 2) return Vec<f64>();

        Vec<f64> rets = Vec<f64>::make(n - 1);
        for (u64 i = 1; i < n; i++) {
            f64 prev = values[i - 1];
            if (prev != 0.0) {
                rets[i - 1] = (values[i] - prev) / prev;
            } else {
                rets[i - 1] = 0.0;
            }
        }
        return rets;
    }

    // -------------------------------------------------------------------
    // volatility — annualized volatility from portfolio return series
    // -------------------------------------------------------------------
    [[nodiscard]] f64 volatility(const TimeSeries<f64>& portfolio_ts) const noexcept {
        Vec<f64> rets = returns(portfolio_ts);
        u64 n = rets.length();
        if (n < 2) return 0.0;

        f64 mean = 0.0;
        for (u64 i = 0; i < n; i++) mean += rets[i];
        mean /= static_cast<f64>(n);

        f64 var = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 diff = rets[i] - mean;
            var += diff * diff;
        }
        var /= static_cast<f64>(n - 1);

        // Annualize: daily -> 252 trading days
        return Math::sqrt(var * 252.0);
    }

    // -------------------------------------------------------------------
    // var — historical VaR from portfolio return series
    // -------------------------------------------------------------------
    [[nodiscard]] f64 var(const TimeSeries<f64>& portfolio_ts,
                          f64 confidence = 0.95) const noexcept {
        Vec<f64> rets = returns(portfolio_ts);
        VaR_Result result = historical_var(rets.slice(), confidence);
        return result.var;
    }

    // -------------------------------------------------------------------
    // sharpe_ratio — annualized Sharpe from portfolio return series
    // -------------------------------------------------------------------
    [[nodiscard]] f64 sharpe_ratio(const TimeSeries<f64>& portfolio_ts,
                                    f64 risk_free = 0.0) const noexcept {
        Vec<f64> rets = returns(portfolio_ts);
        u64 n = rets.length();
        if (n < 2) return 0.0;

        f64 mean = 0.0;
        for (u64 i = 0; i < n; i++) mean += rets[i];
        mean /= static_cast<f64>(n);

        f64 var = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 diff = rets[i] - mean;
            var += diff * diff;
        }
        f64 std = (n > 1) ? Math::sqrt(var / static_cast<f64>(n - 1)) : 0.0;

        if (std < 1e-15) return 0.0;

        f64 daily_rf = risk_free / 252.0;
        return (mean - daily_rf) / std * Math::sqrt(252.0);
    }

    // -------------------------------------------------------------------
    // check_weight_constraints — verify target weights respect bounds
    // -------------------------------------------------------------------
    [[nodiscard]] bool check_weight_constraints(Vec<f64> target_weights,
                                                 f64 min_w, f64 max_w) const noexcept {
        for (u64 i = 0; i < target_weights.length(); i++) {
            if (target_weights[i] < min_w || target_weights[i] > max_w) {
                return false;
            }
        }
        return true;
    }

    // -------------------------------------------------------------------
    // check_turnover — verify turnover between current and target weights
    // Turnover = 0.5 * sum(|target_i - current_i|)
    // -------------------------------------------------------------------
    [[nodiscard]] bool check_turnover(Vec<f64> target_weights,
                                       f64 max_turnover) const noexcept {
        Vec<f64> cur_w = weights();
        u64 n = Math::min(cur_w.length(), target_weights.length());
        if (n == 0) return true;

        f64 turnover = 0.0;
        for (u64 i = 0; i < n; i++) {
            turnover += Math::abs(target_weights[i] - cur_w[i]);
        }
        turnover *= 0.5;
        return turnover <= max_turnover;
    }

    // -------------------------------------------------------------------
    // helper: lookup current price, with fallback to 0
    // -------------------------------------------------------------------
    [[nodiscard]] f64 current_price(String_View instr_id) const noexcept {
        auto opt = market_prices_.try_get(instr_id);
        if (opt.ok()) return **opt;
        return 0.0;
    }

    SPP_RECORD(Portfolio, SPP_FIELD(book_), SPP_FIELD(market_prices_),
               SPP_FIELD(as_of_), SPP_FIELD(base_currency_));
};

// =========================================================================
// RebalanceResult — describes the trades needed to rebalance a portfolio
// =========================================================================
struct RebalanceResult {
    Vec<String_View> trades;           ///< Descriptions of needed trades
    f64              turnover_ = 0.0;  ///< One-way turnover fraction
    f64              estimated_cost_ = 0.0;
    Vec<f64>         target_weights_;
    Vec<f64>         current_weights_;

    SPP_RECORD(RebalanceResult, SPP_FIELD(trades), SPP_FIELD(turnover_),
               SPP_FIELD(estimated_cost_), SPP_FIELD(target_weights_),
               SPP_FIELD(current_weights_));
};

}  // namespace spp::quant
