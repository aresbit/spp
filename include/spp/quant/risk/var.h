#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/math/distributions.h"

namespace spp::quant {

/// Method used for VaR computation.
enum struct VaR_Method : u8 {
    Historical   = 0,
    Parametric   = 1,
    MonteCarlo   = 2,
};

/// Result of a VaR computation at a single confidence level.
/// Convention: var and cvar are positive numbers representing potential LOSS.
struct VaR_Result {
    f64 var        = 0.0;   ///< Value at Risk (positive = loss)
    f64 cvar       = 0.0;   ///< Conditional VaR / Expected Shortfall
    f64 confidence = 0.95;  ///< Confidence level (e.g. 0.95, 0.99)
    VaR_Method method = VaR_Method::Historical;

    SPP_RECORD(VaR_Result, SPP_FIELD(var), SPP_FIELD(cvar),
               SPP_FIELD(confidence), SPP_FIELD(method));
};

// =========================================================================
// historical_var — VaR from historical PnL observations
//
// pnl: vector of profit-and-loss values (positive = gain).
// Returns VaR_Result with method = Historical.
//
// Algorithm:
//   1. Sort PnL in ascending order.
//   2. The VaR at confidence level c is the negative of the (1-c) quantile
//      of the PnL distribution.
//   3. CVaR is the negative mean of all PnL values below the VaR threshold.
// =========================================================================
inline VaR_Result historical_var(Slice<const f64> pnl, f64 confidence = 0.95) noexcept {
    VaR_Result result;
    result.method     = VaR_Method::Historical;
    result.confidence = confidence;

    u64 n = pnl.length();
    if (n == 0) return result;

    // Copy and sort PnL ascending
    Vec<f64> sorted = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) {
        sorted[i] = pnl[i];
    }

    // Insertion sort (acceptable for typical portfolio PnL sizes)
    for (u64 i = 1; i < n; i++) {
        f64 key = sorted[i];
        u64 j   = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    // Quantile index: VaR is the negative of the (1-confidence) quantile
    // e.g. for confidence=0.95, we want the 5th percentile.
    // The index into sorted ascending: floor((1-c) * n) or similar.
    // Use linear interpolation between bracketing values for accuracy.
    f64 alpha = 1.0 - confidence;
    f64 idx_real = alpha * static_cast<f64>(n - 1);
    u64 idx_lo = static_cast<u64>(idx_real);
    u64 idx_hi = idx_lo + 1;
    if (idx_hi >= n) idx_hi = n - 1;

    f64 frac = idx_real - static_cast<f64>(idx_lo);
    f64 var_threshold = sorted[idx_lo] + frac * (sorted[idx_hi] - sorted[idx_lo]);

    // VaR = -quantile (positive = loss)
    result.var = -var_threshold;

    // CVaR = negative mean of PnL below the threshold
    f64 cvar_sum = 0.0;
    u64 cvar_count = 0;
    for (u64 i = 0; i < n; i++) {
        if (sorted[i] <= var_threshold) {
            cvar_sum += sorted[i];
            cvar_count++;
        }
    }

    if (cvar_count > 0) {
        result.cvar = -(cvar_sum / static_cast<f64>(cvar_count));
    } else {
        // Edge case: only one observation at the threshold
        result.cvar = result.var;
    }

    return result;
}

// =========================================================================
// parametric_var — VaR assuming normally distributed returns
//
// portfolio_value: total portfolio notional.
// volatility:       daily return standard deviation (sigma).
// confidence:       VaR confidence level.
// horizon_days:     number of days for which VaR is computed.
//                   1-day VaR is scaled by sqrt(horizon_days).
//
// Formula: VaR = portfolio_value * sigma * z_alpha * sqrt(T)
//   where z_alpha = normal_icdf(confidence)
//         sigma   = daily return std dev
//         T       = horizon_days
//   CVaR   = portfolio_value * sigma * sqrt(T) * phi(z_alpha) / (1-alpha)
//   where phi() is the standard normal PDF.
// =========================================================================
inline VaR_Result parametric_var(f64 portfolio_value, f64 volatility,
                                  f64 confidence = 0.95,
                                  f64 horizon_days = 1.0) noexcept {
    VaR_Result result;
    result.method     = VaR_Method::Parametric;
    result.confidence = confidence;

    if (portfolio_value <= 0.0 || volatility <= 0.0 || horizon_days <= 0.0) {
        return result;
    }

    f64 alpha = 1.0 - confidence;
    f64 z     = dist::normal_icdf(confidence);
    f64 scale = portfolio_value * volatility * Math::sqrt(horizon_days);

    // VaR
    result.var = scale * z;

    // CVaR (Expected Shortfall for normal distribution)
    // ES_alpha = mu + sigma * phi(z_alpha) / alpha
    // phi(z) = exp(-z^2/2) / sqrt(2*pi)
    f64 phi_z = Math::exp(-0.5 * z * z) / Math::sqrt(2.0 * Math::PI64);
    result.cvar = scale * phi_z / alpha;

    return result;
}

// =========================================================================
// var_multi_level — compute VaR at multiple confidence levels from one PnL sample
// =========================================================================
inline Vec<VaR_Result> var_multi_level(Slice<const f64> pnl,
                                        Slice<const f64> confidences) noexcept {
    u64 n_conf = confidences.length();
    Vec<VaR_Result> results = Vec<VaR_Result>::make(n_conf);

    if (pnl.length() == 0) {
        for (u64 i = 0; i < n_conf; i++) {
            results[i].method     = VaR_Method::Historical;
            results[i].confidence = confidences[i];
        }
        return results;
    }

    // Sort once, then query each confidence level
    u64 n = pnl.length();
    Vec<f64> sorted = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) {
        sorted[i] = pnl[i];
    }
    for (u64 i = 1; i < n; i++) {
        f64 key = sorted[i];
        u64 j   = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    for (u64 c = 0; c < n_conf; c++) {
        f64 conf = confidences[c];
        VaR_Result& r = results[c];
        r.method     = VaR_Method::Historical;
        r.confidence = conf;

        f64 alpha = 1.0 - conf;
        f64 idx_real = alpha * static_cast<f64>(n - 1);
        u64 idx_lo = static_cast<u64>(idx_real);
        u64 idx_hi = idx_lo + 1;
        if (idx_hi >= n) idx_hi = n - 1;

        f64 frac = idx_real - static_cast<f64>(idx_lo);
        f64 var_threshold = sorted[idx_lo] + frac * (sorted[idx_hi] - sorted[idx_lo]);

        r.var = -var_threshold;

        f64 cvar_sum = 0.0;
        u64 cvar_count = 0;
        for (u64 i = 0; i < n; i++) {
            if (sorted[i] <= var_threshold) {
                cvar_sum += sorted[i];
                cvar_count++;
            }
        }
        if (cvar_count > 0) {
            r.cvar = -(cvar_sum / static_cast<f64>(cvar_count));
        } else {
            r.cvar = r.var;
        }
    }

    return results;
}

}  // namespace spp::quant
