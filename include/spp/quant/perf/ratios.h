#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <cmath>

namespace spp::quant {

// =========================================================================
// Performance ratios
// All ratios accept periodic (typically daily) returns.  Ratios using
// annualization multiply by sqrt(periods_per_year) where appropriate.
// =========================================================================

// -------------------------------------------------------------------
// sharpe_ratio — excess return per unit of total risk
// Sharpe = (mean(r - rf) / std(r)) * sqrt(periods_per_year)
// -------------------------------------------------------------------
inline f64 sharpe_ratio(Slice<const f64> returns, f64 risk_free = 0.0,
                         f64 periods_per_year = 252.0) noexcept {
    u64 n = returns.length();
    if (n < 2) return 0.0;

    // Compute mean excess return
    f64 mean_excess = 0.0;
    f64 daily_rf = risk_free / periods_per_year;
    for (u64 i = 0; i < n; i++) {
        mean_excess += (returns[i] - daily_rf);
    }
    mean_excess /= static_cast<f64>(n);

    // Compute standard deviation
    f64 var = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 diff = (returns[i] - daily_rf) - mean_excess;
        var += diff * diff;
    }
    f64 std = (n > 1) ? Math::sqrt(var / static_cast<f64>(n - 1)) : 0.0;

    if (std < 1e-15) return 0.0;

    return (mean_excess / std) * Math::sqrt(periods_per_year);
}

// -------------------------------------------------------------------
// sortino_ratio — excess return per unit of downside risk
// Sortino = (mean(r - rf) / downside_std(r)) * sqrt(periods)
// where downside_std only uses returns below rf (or MAR).
// -------------------------------------------------------------------
inline f64 sortino_ratio(Slice<const f64> returns, f64 risk_free = 0.0,
                          f64 periods_per_year = 252.0) noexcept {
    u64 n = returns.length();
    if (n < 2) return 0.0;

    f64 daily_rf = risk_free / periods_per_year;

    // Mean excess return
    f64 mean_excess = 0.0;
    for (u64 i = 0; i < n; i++) {
        mean_excess += (returns[i] - daily_rf);
    }
    mean_excess /= static_cast<f64>(n);

    // Downside variance: only count returns below risk_free
    f64 down_var = 0.0;
    u64 down_count = 0;
    for (u64 i = 0; i < n; i++) {
        if (returns[i] < daily_rf) {
            f64 diff = returns[i] - daily_rf;
            down_var += diff * diff;
            down_count++;
        }
    }

    // Also include zero-deviation observations that equal rf when computing
    // sample std: we use down_count as the divisor for the sample variance.
    if (down_count < 2) return 0.0;

    f64 down_std = Math::sqrt(down_var / static_cast<f64>(down_count - 1));

    if (down_std < 1e-15) return 0.0;

    return (mean_excess / down_std) * Math::sqrt(periods_per_year);
}

// -------------------------------------------------------------------
// calmar_ratio — annualized return / max drawdown
// Calmar = annualized_return / max_drawdown (both as positive decimals)
// -------------------------------------------------------------------
inline f64 calmar_ratio(Slice<const f64> returns, f64 max_drawdown,
                         f64 periods_per_year = 252.0) noexcept {
    u64 n = returns.length();
    if (n == 0 || max_drawdown < 1e-15) return 0.0;

    // Annualized geometric return
    f64 cumul = 1.0;
    for (u64 i = 0; i < n; i++) {
        cumul *= (1.0 + returns[i]);
    }
    f64 total_return = cumul - 1.0;

    if (total_return <= -1.0 && max_drawdown > 0.0) return -1.0;

    // If the portfolio went to zero or negative, total_return+1 <= 0
    f64 base = total_return + 1.0;
    if (base <= 0.0) return -1.0;

    f64 n_years = static_cast<f64>(n) / periods_per_year;
    if (n_years < 1e-15) return 0.0;

    f64 ann_ret = Math::pow(base, 1.0 / n_years) - 1.0;

    return ann_ret / max_drawdown;
}

// -------------------------------------------------------------------
// information_ratio — active return / tracking error vs benchmark
// IR = mean(r - b) / std(r - b) * sqrt(periods)
// -------------------------------------------------------------------
inline f64 information_ratio(Slice<const f64> returns,
                              Slice<const f64> benchmark) noexcept {
    u64 n = Math::min(returns.length(), benchmark.length());
    if (n < 2) return 0.0;

    // Active returns
    f64 mean_active = 0.0;
    for (u64 i = 0; i < n; i++) {
        mean_active += (returns[i] - benchmark[i]);
    }
    mean_active /= static_cast<f64>(n);

    f64 var_active = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 diff = (returns[i] - benchmark[i]) - mean_active;
        var_active += diff * diff;
    }
    f64 std_active = (n > 1) ? Math::sqrt(var_active / static_cast<f64>(n - 1)) : 0.0;

    if (std_active < 1e-15) return 0.0;

    return (mean_active / std_active) * Math::sqrt(252.0);
}

// -------------------------------------------------------------------
// treynor_ratio — excess return per unit of systematic (beta) risk
// Treynor = (mean(r - rf) / beta) * periods_per_year
// -------------------------------------------------------------------
inline f64 treynor_ratio(Slice<const f64> returns, f64 beta,
                          f64 risk_free = 0.0) noexcept {
    u64 n = returns.length();
    if (n == 0 || Math::abs(beta) < 1e-15) return 0.0;

    f64 mean_excess = 0.0;
    f64 daily_rf = risk_free / 252.0;
    for (u64 i = 0; i < n; i++) {
        mean_excess += (returns[i] - daily_rf);
    }
    mean_excess /= static_cast<f64>(n);

    return (mean_excess / beta) * 252.0;
}

// -------------------------------------------------------------------
// omega_ratio — probability-weighted gain / loss ratio
// Omega = integral_{threshold}^{inf} (1-F(x)) dx /
//         integral_{-inf}^{threshold} F(x) dx
//
// Discrete approximation:
//   sum(max(r_t - threshold, 0)) / sum(max(threshold - r_t, 0))
// -------------------------------------------------------------------
inline f64 omega_ratio(Slice<const f64> returns, f64 threshold = 0.0) noexcept {
    u64 n = returns.length();
    if (n == 0) return 1.0;

    f64 gains = 0.0;
    f64 losses = 0.0;

    for (u64 i = 0; i < n; i++) {
        f64 diff = returns[i] - threshold;
        if (diff > 0.0) {
            gains += diff;
        } else {
            losses += -diff;
        }
    }

    if (losses < 1e-15) {
        // No losses: infinite omega, return large number
        return gains > 0.0 ? 1e15 : 1.0;
    }

    return gains / losses;
}

// -------------------------------------------------------------------
// win_rate — fraction of periods with positive returns
// -------------------------------------------------------------------
inline f64 win_rate(Slice<const f64> returns) noexcept {
    u64 n = returns.length();
    if (n == 0) return 0.0;

    u64 wins = 0;
    for (u64 i = 0; i < n; i++) {
        if (returns[i] > 0.0) wins++;
    }
    return static_cast<f64>(wins) / static_cast<f64>(n);
}

// -------------------------------------------------------------------
// profit_factor — sum of gains / sum of |losses|
// -------------------------------------------------------------------
inline f64 profit_factor(Slice<const f64> returns) noexcept {
    u64 n = returns.length();
    if (n == 0) return 0.0;

    f64 total_gains = 0.0;
    f64 total_losses = 0.0;

    for (u64 i = 0; i < n; i++) {
        if (returns[i] > 0.0) {
            total_gains += returns[i];
        } else {
            total_losses += -returns[i];
        }
    }

    if (total_losses < 1e-15) {
        return total_gains > 0.0 ? 1e15 : 0.0;
    }

    return total_gains / total_losses;
}

}  // namespace spp::quant
