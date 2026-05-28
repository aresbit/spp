#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/data/market_data.h"

namespace spp::quant {

/// A single stress scenario — shocks applied to market factors.
/// All shocks are additive/multiplicative (not percentage points):
///   spot_shock:  multiplicative, e.g. -0.20 = 20% decline
///   vol_shock:   additive, e.g. +0.10 = vol increases by 10 percentage points
///   rate_shock:  additive, e.g. +0.0050 = 50bp rate increase
///   fx_shock:    multiplicative, e.g. -0.10 = 10% FX depreciation
struct StressScenario {
    String_View name_              = ""_v;
    f64         spot_shock_        = 0.0;
    f64         vol_shock_         = 0.0;
    f64         rate_shock_        = 0.0;
    f64         fx_shock_          = 0.0;
    f64         correlation_shock_ = 0.0;

    SPP_RECORD(StressScenario, SPP_FIELD(name_), SPP_FIELD(spot_shock_),
               SPP_FIELD(vol_shock_), SPP_FIELD(rate_shock_), SPP_FIELD(fx_shock_),
               SPP_FIELD(correlation_shock_));
};

// =========================================================================
// Pre-built historical and hypothetical stress scenarios.
// =========================================================================
namespace Scenarios {

inline const StressScenario flash_crash_2010() noexcept {
    return {"Flash Crash 2010"_v, -0.09, 0.40, 0.0, 0.0, 0.0};
}

inline const StressScenario gfc_2008() noexcept {
    return {"Global Financial Crisis 2008"_v, -0.50, 0.80, -0.02, 0.0, 0.30};
}

inline const StressScenario taper_tantrum_2013() noexcept {
    return {"Taper Tantrum 2013"_v, 0.0, 0.0, 0.01, 0.0, 0.0};
}

inline const StressScenario covid_crash_2020() noexcept {
    return {"COVID Crash 2020"_v, -0.34, 1.20, -0.015, 0.0, 0.50};
}

inline const StressScenario black_monday_1987() noexcept {
    return {"Black Monday 1987"_v, -0.20, 1.00, 0.0, 0.0, 0.0};
}

inline const StressScenario dotcom_burst_2000() noexcept {
    return {"Dotcom Burst 2000"_v, -0.30, 0.30, 0.0, 0.0, 0.0};
}

inline const StressScenario china_2015_crash() noexcept {
    return {"China 2015 Crash"_v, -0.40, 0.50, 0.0, 0.0, 0.0};
}

}  // namespace Scenarios

// =========================================================================
// apply_stress — create a shocked copy of MarketData
//
// Applies multiplicative spot shock: spot' = spot * (1 + shock)
// Vol shock passed through so the pricer lambda can use it.
// Rate shock added to dividend_yield_ / repo_rate_ fields.
// =========================================================================
inline MarketData apply_stress(const MarketData& base,
                                const StressScenario& scenario) noexcept {
    MarketData shocked = base;

    // Spot shock: multiplicative
    if (shocked.spot_price_.ok()) {
        shocked.spot_price_ = *shocked.spot_price_ * (1.0 + scenario.spot_shock_);
    }

    // Vol shock is stored in dividend_yield_ as a proxy for external vol param.
    // Real usage: the pricer lambda should capture the vol variable and
    // the stress test should bump it directly.  We store the additive shock
    // so the caller can query it.
    // (dividend yield rate shock already handled below — we prioritize rate there.)

    // Rate shock: additive to rate-related fields
    if (shocked.dividend_yield_.ok() && scenario.rate_shock_ != 0.0) {
        shocked.dividend_yield_ = *shocked.dividend_yield_ + scenario.rate_shock_;
    }
    if (shocked.repo_rate_.ok() && scenario.rate_shock_ != 0.0) {
        shocked.repo_rate_ = *shocked.repo_rate_ + scenario.rate_shock_;
    }

    // FX shock: multiplicative
    if (shocked.fx_spot_.ok()) {
        shocked.fx_spot_ = *shocked.fx_spot_ * (1.0 + scenario.fx_shock_);
    }

    // Correlation shock: stored in MarketData — no direct field available.
    // The pricer lambda should interpret this externally.
    (void)scenario.correlation_shock_;

    return shocked;
}

/// Result of running a single stress scenario on a portfolio.
struct StressResult {
    StressScenario scenario;
    f64            pnl_impact = 0.0;  ///< PnL under scenario (negative = loss)
    f64            pnl_pct    = 0.0;  ///< Percentage of portfolio value

    SPP_RECORD(StressResult, SPP_FIELD(scenario), SPP_FIELD(pnl_impact),
               SPP_FIELD(pnl_pct));
};

// =========================================================================
// stress_test — run a suite of stress scenarios against a portfolio
//
// The stress pricer is a callable:
//   f64(const MarketData&, const StressScenario&)
// that returns the PnL (positive = gain) of the portfolio under the
// shocked market data AND vol/rate shocks from the scenario.
//
// pnl_impact = pricer(stressed_mkt, scenario)
// pnl_pct    = pnl_impact / base_value (if base_value != 0)
// =========================================================================
template <typename StressPricer>
Vec<StressResult> stress_test(StressPricer&& pricer,
                               const MarketData& base_mkt,
                               Slice<const StressScenario> scenarios,
                               f64 base_value = 1.0) noexcept {
    u64 n = scenarios.length();
    Vec<StressResult> results = Vec<StressResult>::make(n);

    for (u64 i = 0; i < n; i++) {
        const StressScenario& sc = scenarios[i];
        MarketData shocked = apply_stress(base_mkt, sc);

        StressResult& sr = results[i];
        sr.scenario   = sc;
        sr.pnl_impact = pricer(shocked, sc);
        sr.pnl_pct    = (base_value != 0.0) ? sr.pnl_impact / base_value : 0.0;
    }

    return results;
}

}  // namespace spp::quant
