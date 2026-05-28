#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <cmath>

namespace spp::quant {

// =========================================================================
// Returns computation utilities
// =========================================================================

// -------------------------------------------------------------------
// simple_returns — r_t = (P_t - P_{t-1}) / P_{t-1}
// Returns N-1 values for N input prices.
// -------------------------------------------------------------------
inline Vec<f64> simple_returns(Slice<const f64> prices) noexcept {
    u64 n = prices.length();
    if (n < 2) return Vec<f64>();

    Vec<f64> rets = Vec<f64>::make(n - 1);
    for (u64 i = 1; i < n; i++) {
        if (prices[i - 1] != 0.0) {
            rets[i - 1] = (prices[i] - prices[i - 1]) / prices[i - 1];
        } else {
            rets[i - 1] = 0.0;
        }
    }
    return rets;
}

// -------------------------------------------------------------------
// log_returns — r_t = ln(P_t / P_{t-1})
// Returns N-1 values for N input prices.
// -------------------------------------------------------------------
inline Vec<f64> log_returns(Slice<const f64> prices) noexcept {
    u64 n = prices.length();
    if (n < 2) return Vec<f64>();

    Vec<f64> rets = Vec<f64>::make(n - 1);
    for (u64 i = 1; i < n; i++) {
        if (prices[i] > 0.0 && prices[i - 1] > 0.0) {
            rets[i - 1] = Math::log(prices[i] / prices[i - 1]);
        } else {
            rets[i - 1] = 0.0;
        }
    }
    return rets;
}

// -------------------------------------------------------------------
// cumulative_return — total return over a series of periodic returns
// R_cumul = prod(1 + r_t) - 1
// -------------------------------------------------------------------
inline f64 cumulative_return(Slice<const f64> returns) noexcept {
    u64 n = returns.length();
    f64 cumul = 1.0;
    for (u64 i = 0; i < n; i++) {
        cumul *= (1.0 + returns[i]);
    }
    return cumul - 1.0;
}

// -------------------------------------------------------------------
// annualized_return — geometric annualization of daily returns
// R_ann = (prod(1 + r_t))^(periods / n) - 1
// Default: 252 trading days per year.
// -------------------------------------------------------------------
inline f64 annualized_return(Slice<const f64> daily_returns,
                              f64 periods_per_year = 252.0) noexcept {
    u64 n = daily_returns.length();
    if (n == 0) return 0.0;

    f64 cumul = cumulative_return(daily_returns);
    f64 total_return = 1.0 + cumul;

    if (total_return <= 0.0) return -1.0;

    f64 n_years = static_cast<f64>(n) / periods_per_year;
    if (n_years < 1e-15) return 0.0;

    return Math::pow(total_return, 1.0 / n_years) - 1.0;
}

// -------------------------------------------------------------------
// annualized_volatility — annualized std dev of daily returns
// sigma_ann = sigma_daily * sqrt(periods_per_year)
// Default: 252 trading days per year.
// -------------------------------------------------------------------
inline f64 annualized_volatility(Slice<const f64> daily_returns,
                                  f64 periods_per_year = 252.0) noexcept {
    u64 n = daily_returns.length();
    if (n < 2) return 0.0;

    f64 mean = 0.0;
    for (u64 i = 0; i < n; i++) mean += daily_returns[i];
    mean /= static_cast<f64>(n);

    f64 var = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 diff = daily_returns[i] - mean;
        var += diff * diff;
    }
    var /= static_cast<f64>(n - 1);

    return Math::sqrt(var) * Math::sqrt(periods_per_year);
}

// -------------------------------------------------------------------
// excess_returns — r_t - benchmark_t
// If benchmark has fewer elements, reuses the last value (constant
// risk-free rate pattern).
// -------------------------------------------------------------------
inline Vec<f64> excess_returns(Slice<const f64> returns,
                                Slice<const f64> benchmark_or_rf) noexcept {
    u64 n = returns.length();
    if (n == 0) return Vec<f64>();

    u64 bn = benchmark_or_rf.length();
    Vec<f64> excess = Vec<f64>::make(n);

    for (u64 i = 0; i < n; i++) {
        f64 bm = (i < bn) ? benchmark_or_rf[i] : benchmark_or_rf[bn - 1];
        excess[i] = returns[i] - bm;
    }
    return excess;
}

}  // namespace spp::quant
