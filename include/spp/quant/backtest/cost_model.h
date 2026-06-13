#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

namespace spp::quant::backtest {

// =========================================================================
// AlmgrenChrissImpact — AC market impact model (Almgren & Chriss 2000)
//
// Temporary impact (dissipates):  eta * sigma * (X / (V * T))^beta * sign(X)
// Permanent impact (stays):       gamma * sigma * X / (V * T)
//
// where:
//   X     = order size (shares)
//   V     = average daily volume (shares)
//   T     = execution time fraction (0-1, 1 = full day)
//   sigma = daily volatility
//   eta   = temporary impact coefficient
//   gamma = permanent impact coefficient
//   beta  = power-law exponent (0.5 = square-root model)
// =========================================================================
struct AlmgrenChrissImpact {
    f64 eta_          = 0.1;    // temporary impact coefficient
    f64 gamma_        = 0.01;   // permanent impact coefficient
    f64 beta_         = 0.5;    // power-law exponent
    f64 sigma_        = 0.2;    // daily volatility
    f64 daily_volume_ = 1e6;    // average daily volume (shares)

    struct ImpactResult {
        f64 temporary_cost_pct;   // as fraction of price
        f64 permanent_cost_pct;   // as fraction of price
        f64 total_cost_pct;       // temporary + permanent
    };

    // -------------------------------------------------------------------
    // calculate — estimate market impact for an order
    //
    // order_size:             number of shares to trade
    // execution_time_fraction: fraction of the day for execution (0-1)
    // -------------------------------------------------------------------
    [[nodiscard]] ImpactResult calculate(f64 order_size,
                                          f64 execution_time_fraction = 1.0) const noexcept {
        ImpactResult result{0.0, 0.0, 0.0};

        if (daily_volume_ <= 0.0 || execution_time_fraction <= 0.0 || sigma_ <= 0.0) {
            return result;
        }

        // Participation rate: X / (V * T)
        f64 participation = Math::abs(order_size) /
                            (daily_volume_ * execution_time_fraction);

        // Permanent impact (proportional to participation rate)
        result.permanent_cost_pct = gamma_ * sigma_ * participation;

        // Temporary impact (power-law in participation rate)
        // Temporary impact: eta * sigma * participation^beta
        result.temporary_cost_pct = eta_ * sigma_ *
                                    Math::pow(participation, beta_);

        result.total_cost_pct = result.temporary_cost_pct + result.permanent_cost_pct;

        return result;
    }
};

// =========================================================================
// FullCostModel — complete transaction cost model
// Combines commissions, spread costs, and market impact.
// =========================================================================
struct FullCostModel {
    f64 commission_per_share_ = 0.0;      // e.g. $0.005 / share
    f64 commission_per_trade_ = 5.0;      // e.g. $5 flat per trade
    f64 commission_pct_       = 0.001;    // 10 bps of notional
    f64 min_commission_       = 0.0;      // minimum commission per trade
    f64 spread_pct_           = 0.0005;   // half-spread (5 bps)
    AlmgrenChrissImpact impact_;

    struct CostBreakdown {
        f64 commission;       // absolute $
        f64 spread_cost;      // absolute $ (half-spread * notional)
        f64 market_impact;    // absolute $ from AC model
        f64 total;            // sum of above
        f64 total_pct;        // total as fraction of notional
    };

    // -------------------------------------------------------------------
    // calculate — compute the full cost breakdown for a trade
    //
    // price:             execution price per share
    // quantity:          number of shares (> 0, use abs internally)
    // daily_volatility:  for AC model (default: 0.2 = 20% annual)
    // daily_volume_amt:  ADV in shares (default: 1e6)
    // execution_time:    fraction of day to execute (default: 1.0)
    // -------------------------------------------------------------------
    [[nodiscard]] CostBreakdown calculate(f64 price, f64 quantity,
                                           f64 daily_volatility = 0.2,
                                           f64 daily_volume_amt  = 1e6,
                                           f64 execution_time     = 1.0) const noexcept {
        CostBreakdown cb{};
        if (price <= 0.0 || quantity == 0.0) return cb;

        f64 abs_qty      = Math::abs(quantity);
        f64 notional     = abs_qty * price;

        // Commission: per-share + per-trade + percentage
        cb.commission = commission_per_share_ * abs_qty
                      + commission_per_trade_
                      + commission_pct_ * notional;
        if (cb.commission < min_commission_) cb.commission = min_commission_;

        // Spread cost: half-spread * notional (crossing the spread)
        cb.spread_cost = spread_pct_ * notional;

        // Market impact: use AC model
        // Temporarily override AC params for this call
        AlmgrenChrissImpact ac = impact_;
        ac.sigma_        = daily_volatility;
        ac.daily_volume_ = daily_volume_amt;

        auto impact = ac.calculate(abs_qty, execution_time);
        cb.market_impact = impact.total_cost_pct * price * abs_qty;

        cb.total     = cb.commission + cb.spread_cost + cb.market_impact;
        cb.total_pct = (notional > 0.0) ? cb.total / notional : 0.0;

        return cb;
    }
};

} // namespace spp::quant::backtest

SPP_NAMED_RECORD(::spp::quant::backtest::AlmgrenChrissImpact, "AlmgrenChrissImpact",
                 SPP_FIELD(eta_), SPP_FIELD(gamma_), SPP_FIELD(beta_),
                 SPP_FIELD(sigma_), SPP_FIELD(daily_volume_));

SPP_NAMED_RECORD(::spp::quant::backtest::FullCostModel, "FullCostModel",
                 SPP_FIELD(commission_per_share_), SPP_FIELD(commission_per_trade_),
                 SPP_FIELD(commission_pct_), SPP_FIELD(min_commission_),
                 SPP_FIELD(spread_pct_));
