#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/math/distributions.h"
#include "spp/quant/math/statistics.h"

namespace spp::quant::calib {

// =========================================================================
// CalibrationResult — output of a calibration run
// =========================================================================

template<typename Params>
struct CalibrationResult {
    Params   params_;
    f64      error_          = Limits<f64>::max();
    u64      iterations_     = 0;
    u64      function_evals_ = 0;
    bool     converged_      = false;
    Vec<f64> error_history_;
};

// =========================================================================
// Calibrator — generic model calibration engine
// =========================================================================
//
// Uses derivative-free optimization (Nelder-Mead) by default. The objective
// function is the sum of squared errors between model prices and market quotes.

template<typename Model, typename Params>
struct Calibrator {
    /// Calibrate model parameters to market data.
    ///
    /// pricer:    (const Model&, const Params&, f64 market_data_i) -> f64
    ///            Computes the theoretical price for the i-th market quote.
    ///
    /// error_fn:  (f64 theoretical, f64 market) -> f64
    ///            Computes the error between theoretical and market values.
    ///
    /// The optimizer minimizes: sum_i error_fn(pricer(model, params, data_i), quote_i)

    template<typename PricerFn, typename ErrorFn>
    [[nodiscard]] static CalibrationResult<Params> calibrate(
        const Model& model,
        const Params& initial_params,
        Slice<const f64> market_quotes,
        Slice<const f64> lower_bounds,
        Slice<const f64> upper_bounds,
        PricerFn pricer,
        ErrorFn error_fn,
        f64 tolerance = 1e-6,
        u64 max_iterations = 500)
    {
        u64 n_quotes = market_quotes.length();
        CalibrationResult<Params> result;
        if (n_quotes == 0) return result;

        // Build objective function: params -> total error
        auto objective = [&](Slice<const f64> param_vec) -> f64 {
            Params p = vec_to_params(param_vec);
            f64 total_error = 0.0;
            for (u64 i = 0; i < n_quotes; i++) {
                f64 theoretical = pricer(model, p, i);
                f64 err = error_fn(theoretical, market_quotes[i]);
                total_error += err * err;
            }
            return Math::sqrt(total_error / static_cast<f64>(n_quotes));
        };

        return calibrate_obj(objective, initial_params,
                             lower_bounds, upper_bounds, tolerance, max_iterations);
    }

    /// Calibrate using an objective function directly.
    ///
    /// objective: (Slice<const f64> params) -> f64 (scalar error to minimize)

    [[nodiscard]] static CalibrationResult<Params> calibrate_obj(
        auto objective,
        const Params& initial_params,
        Slice<const f64> lower_bounds,
        Slice<const f64> upper_bounds,
        f64 tolerance = 1e-6,
        u64 max_iterations = 500)
    {
        CalibrationResult<Params> result;

        u64 n_params = lower_bounds.length();
        if (n_params == 0 || upper_bounds.length() != n_params)
            return result;

        // Convert Params to Vec<f64>
        Vec<f64> x0 = params_to_vec(initial_params);

        // Wrapped objective with bounds enforcement via penalty
        auto bounded_obj = [&](Slice<const f64> x) -> f64 {
            f64 penalty = 0.0;
            for (u64 i = 0; i < n_params; i++) {
                if (x[i] < lower_bounds[i])
                    penalty += (lower_bounds[i] - x[i]) * (lower_bounds[i] - x[i]) * 1e6;
                else if (x[i] > upper_bounds[i])
                    penalty += (x[i] - upper_bounds[i]) * (x[i] - upper_bounds[i]) * 1e6;
            }
            return objective(x) + penalty;
        };

        // Nelder-Mead simplex optimization
        // Standard NM parameters
        constexpr f64 alpha = 1.0;   // reflection
        constexpr f64 gamma = 2.0;   // expansion
        constexpr f64 rho   = 0.5;   // contraction
        constexpr f64 sigma = 0.5;   // shrink

        u64 m = n_params + 1;
        // Initialize simplex
        Vec<Vec<f64>> simplex;
        simplex.push(x0);
        for (u64 i = 0; i < n_params; i++) {
            Vec<f64> pt = x0.clone();
            f64 perturb = 0.05 * (upper_bounds[i] - lower_bounds[i]);
            if (perturb < 1e-6) perturb = 0.01;
            pt[i] += perturb;
            if (pt[i] > upper_bounds[i]) pt[i] = upper_bounds[i];
            simplex.push(spp::move(pt));
        }

        Vec<f64> fvals = Vec<f64>::make(m);
        for (u64 i = 0; i < m; i++)
            fvals[i] = bounded_obj(simplex[i].slice());

        for (u64 iter = 0; iter < max_iterations; iter++) {
            // Sort simplex by function value (ascending)
            for (u64 i = 1; i < m; i++) {
                for (u64 j = i; j > 0; j--) {
                    if (fvals[j] < fvals[j - 1]) {
                        // Swap
                        f64 tmp_f = fvals[j];
                        fvals[j] = fvals[j - 1];
                        fvals[j - 1] = tmp_f;
                        Vec<f64> tmp_v = spp::move(simplex[j]);
                        simplex[j] = spp::move(simplex[j - 1]);
                        simplex[j - 1] = spp::move(tmp_v);
                    } else break;
                }
            }

            result.error_history_.push(fvals[0]);
            result.function_evals_ += m;

            // Check convergence: std of function values
            f64 fmean = 0.0;
            for (u64 i = 0; i < m; i++) fmean += fvals[i];
            fmean /= static_cast<f64>(m);
            f64 fstd = 0.0;
            for (u64 i = 0; i < m; i++) {
                f64 diff = fvals[i] - fmean;
                fstd += diff * diff;
            }
            fstd = Math::sqrt(fstd / static_cast<f64>(m));
            if (fstd < tolerance) {
                result.converged_ = true;
                break;
            }

            // Compute centroid of all points except the worst (index m-1)
            Vec<f64> centroid = Vec<f64>::make(n_params);
            for (u64 i = 0; i < n_params; i++) centroid[i] = 0.0;
            for (u64 i = 0; i < m - 1; i++) {
                for (u64 j = 0; j < n_params; j++)
                    centroid[j] += simplex[i][j];
            }
            for (u64 j = 0; j < n_params; j++)
                centroid[j] /= static_cast<f64>(m - 1);

            // Reflect
            Vec<f64> reflected = Vec<f64>::make(n_params);
            for (u64 j = 0; j < n_params; j++)
                reflected[j] = centroid[j] + alpha * (centroid[j] - simplex[m - 1][j]);
            f64 f_reflected = bounded_obj(reflected.slice());
            result.function_evals_++;

            if (f_reflected < fvals[0]) {
                // Reflection is better than best: try expansion
                Vec<f64> expanded = Vec<f64>::make(n_params);
                for (u64 j = 0; j < n_params; j++)
                    expanded[j] = centroid[j] + gamma * (reflected[j] - centroid[j]);
                f64 f_expanded = bounded_obj(expanded.slice());
                result.function_evals_++;

                if (f_expanded < f_reflected) {
                    simplex[m - 1] = spp::move(expanded);
                    fvals[m - 1] = f_expanded;
                } else {
                    simplex[m - 1] = spp::move(reflected);
                    fvals[m - 1] = f_reflected;
                }
            } else if (f_reflected < fvals[m - 2]) {
                // Reflection is better than second-worst: accept
                simplex[m - 1] = spp::move(reflected);
                fvals[m - 1] = f_reflected;
            } else {
                // Reflection is worse: try contraction
                Vec<f64> contracted = Vec<f64>::make(n_params);
                if (f_reflected < fvals[m - 1]) {
                    // Outside contraction
                    for (u64 j = 0; j < n_params; j++)
                        contracted[j] = centroid[j] + rho * (reflected[j] - centroid[j]);
                } else {
                    // Inside contraction
                    for (u64 j = 0; j < n_params; j++)
                        contracted[j] = centroid[j] + rho * (simplex[m - 1][j] - centroid[j]);
                }
                f64 f_contracted = bounded_obj(contracted.slice());
                result.function_evals_++;

                if (f_contracted < Math::min(f_reflected, fvals[m - 1])) {
                    simplex[m - 1] = spp::move(contracted);
                    fvals[m - 1] = f_contracted;
                } else {
                    // Shrink: replace all points except the best
                    for (u64 i = 1; i < m; i++) {
                        for (u64 j = 0; j < n_params; j++)
                            simplex[i][j] = simplex[0][j] + sigma * (simplex[i][j] - simplex[0][j]);
                        fvals[i] = bounded_obj(simplex[i].slice());
                        result.function_evals_++;
                    }
                }
            }

            result.iterations_ = iter + 1;
        }

        result.params_ = vec_to_params(simplex[0].slice());
        result.error_  = fvals[0];
        return result;
    }

    // ---- Params <-> Vec<f64> conversion helpers ----
    // Default implementation: assumes Params is a simple aggregate of f64 fields.
    // Specialize for custom types as needed.
    // We implement a generic approach using field count.

private:
    template<typename T>
    [[nodiscard]] static Vec<f64> params_to_vec(const T& p) {
        // Use SPP reflection or manual conversion.
        // For now, we rely on the caller to provide a specialization
        // or define this per type. As a fallback, assume sequential f64 fields.
        // This is a placeholder — real usage requires specialization.
        Vec<f64> v;
        params_to_vec_impl(p, v);
        return v;
    }

    template<typename T>
    [[nodiscard]] static T vec_to_params(Slice<const f64> v) {
        T p;
        vec_to_params_impl(v, p);
        return p;
    }
};

// =========================================================================
// Helper: params_to_vec / vec_to_params for common calibration params
// =========================================================================

// [UNSPECIFIED] These overloads must be updated when new parameter types are added.
// The current approach uses ad-hoc overloads rather than SPP reflection macros.

inline Vec<f64> params_to_vec_impl(const struct HestonParams& p, Vec<f64>& v) {
    v.push(p.kappa_); v.push(p.theta_); v.push(p.sigma_);
    v.push(p.rho_);   v.push(p.v0_);
    return v;
}
inline void vec_to_params_impl(Slice<const f64> v, struct HestonParams& p) {
    p.kappa_ = v[0]; p.theta_ = v[1]; p.sigma_ = v[2];
    p.rho_   = v[3]; p.v0_    = v[4];
}

inline Vec<f64> params_to_vec_impl(const struct SABRParams& p, Vec<f64>& v) {
    v.push(p.alpha_); v.push(p.beta_); v.push(p.rho_); v.push(p.nu_);
    return v;
}
inline void vec_to_params_impl(Slice<const f64> v, struct SABRParams& p) {
    p.alpha_ = v[0]; p.beta_  = v[1]; p.rho_   = v[2]; p.nu_    = v[3];
}

// =========================================================================
// CalibrationObjectives — common error functions for calibration
// =========================================================================

struct CalibrationObjectives {
    /// Mean Squared Error: (1/N) * sum (model_i - market_i)^2
    [[nodiscard]] static f64 mse(Slice<const f64> model_values,
                                  Slice<const f64> market_values) {
        u64 n = Math::min(model_values.length(), market_values.length());
        if (n == 0) return 0.0;
        f64 sum = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 diff = model_values[i] - market_values[i];
            sum += diff * diff;
        }
        return sum / static_cast<f64>(n);
    }

    /// Weighted MSE: each instrument weighted by 1/vega or 1/bid_ask spread.
    [[nodiscard]] static f64 weighted_mse(Slice<const f64> model_values,
                                           Slice<const f64> market_values,
                                           Slice<const f64> weights) {
        u64 n = Math::min(
            Math::min(model_values.length(), market_values.length()),
            weights.length());
        if (n == 0) return 0.0;
        f64 sum = 0.0;
        f64 w_sum = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 diff = model_values[i] - market_values[i];
            sum += weights[i] * diff * diff;
            w_sum += weights[i];
        }
        return (w_sum > 1e-15) ? sum / w_sum : 0.0;
    }

    /// Root Mean Squared Implied Volatility Error.
    /// The standard objective for options calibration.
    [[nodiscard]] static f64 implied_vol_error(Slice<const f64> model_vols,
                                                Slice<const f64> market_vols) {
        u64 n = Math::min(model_vols.length(), market_vols.length());
        if (n == 0) return 0.0;
        f64 sum = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 diff = model_vols[i] - market_vols[i];
            sum += diff * diff;
        }
        return Math::sqrt(sum / static_cast<f64>(n));
    }

    /// Mean Absolute Percentage Error.
    [[nodiscard]] static f64 percentage_error(Slice<const f64> model_values,
                                               Slice<const f64> market_values) {
        u64 n = Math::min(model_values.length(), market_values.length());
        if (n == 0) return 0.0;
        f64 sum = 0.0;
        for (u64 i = 0; i < n; i++) {
            if (Math::abs(market_values[i]) > 1e-15) {
                f64 pct = Math::abs((model_values[i] - market_values[i]) / market_values[i]);
                sum += pct;
            }
        }
        return sum / static_cast<f64>(n);
    }

    /// Vega-weighted error: weight each option by 1/vega, giving more
    /// importance to ATM options where vega is highest.
    [[nodiscard]] static f64 vega_weighted_error(Slice<const f64> model_vols,
                                                  Slice<const f64> market_vols,
                                                  Slice<const f64> vegas) {
        return weighted_mse(model_vols, market_vols, vegas);
    }
};

// =========================================================================
// HestonParams — Heston (1993) stochastic volatility model parameters
// =========================================================================
//
// dS_t = mu * S_t * dt + sqrt(v_t) * S_t * dW^S_t
// dv_t = kappa * (theta - v_t) * dt + sigma * sqrt(v_t) * dW^v_t
// d<W^S, W^v>_t = rho * dt
//
// Feller condition: 2 * kappa * theta > sigma^2 ensures v_t > 0

struct HestonParams {
    f64 kappa_ = 2.0;   ///< mean reversion speed
    f64 theta_ = 0.04;  ///< long-term variance
    f64 sigma_ = 0.3;   ///< volatility of variance (vol of vol)
    f64 rho_   = -0.7;  ///< correlation between spot and variance
    f64 v0_    = 0.04;  ///< initial variance

    SPP_RECORD(HestonParams, SPP_FIELD(kappa_), SPP_FIELD(theta_),
               SPP_FIELD(sigma_), SPP_FIELD(rho_), SPP_FIELD(v0_));

    /// Check Feller condition: 2 * kappa * theta > sigma^2
    [[nodiscard]] bool feller_ok() const noexcept {
        return 2.0 * kappa_ * theta_ > sigma_ * sigma_;
    }
};

// =========================================================================
// SABRParams — SABR (Hagan et al. 2002) stochastic volatility model
// =========================================================================
//
// dF_t = alpha_t * F_t^beta * dW^1_t
// dalpha_t = nu * alpha_t * dW^2_t
// d<W^1, W^2>_t = rho * dt
//
// beta: CEV exponent (0 <= beta <= 1)
//   beta = 0: normal (Bachelier)
//   beta = 1: lognormal (Black)
//   beta = 0.5: CIR-like (common for interest rates)

struct SABRParams {
    f64 alpha_ = 0.2;   ///< initial volatility level
    f64 beta_  = 0.5;   ///< CEV exponent (usually fixed during calibration)
    f64 rho_   = -0.3;  ///< volatility-forward correlation (skew)
    f64 nu_    = 0.4;   ///< volatility of volatility (smile convexity)

    SPP_RECORD(SABRParams, SPP_FIELD(alpha_), SPP_FIELD(beta_),
               SPP_FIELD(rho_), SPP_FIELD(nu_));

    // =====================================================================
    // Hagan et al. (2002) implied volatility expansion.
    //
    // The asymptotic expansion for Black implied vol sigma_B(K, F, T):
    //
    // sigma_B = (alpha / (F^(1-beta))) *
    //   (z / x(z)) *
    //   { 1 + [ ((1-beta)^2 * alpha^2) / (24 * (F^(2-2*beta)))
    //         + (rho * beta * nu * alpha) / (4 * (F^(1-beta)))
    //         + ((2-3*rho^2) * nu^2) / 24 ] * T + ... }
    //
    // where:
    //   z = (nu / alpha) * (F^(1-beta)) * ln(F/K)           [for beta < 1]
    //   z = (nu / alpha) * (F * K)^((1-beta)/2) * ln(F/K)  [lognormal corr]
    //   x(z) = ln( (sqrt(1-2*rho*z+z^2) + z - rho) / (1-rho) )
    //
    // The full formula below uses the corrected version from
    // Hagan et al. (2002), with the Obloj (2008) correction for
    // the ATM case (when F = K, z -> 0, x(z)/z -> 1).
    // =====================================================================

    [[nodiscard]] static f64 implied_vol(f64 forward, f64 strike,
                                          f64 expiry, f64 alpha, f64 beta,
                                          f64 rho, f64 nu) {
        // Guard against invalid inputs
        if (forward <= 0.0 || strike <= 0.0 || expiry <= 0.0) return 0.0;
        if (alpha <= 0.0 || nu < 0.0) return 0.0;
        if (beta < 0.0 || beta > 1.0) return 0.0;
        if (rho < -0.9999) rho = -0.9999;
        if (rho > 0.9999) rho = 0.9999;

        // ATM special case: when strike == forward, use the ATM formula
        if (Math::abs(forward - strike) < 1e-12 * forward) {
            return implied_vol_atm(forward, expiry, alpha, beta, rho, nu);
        }

        f64 f_avg = Math::pow(forward * strike, (1.0 - beta) / 2.0);
        f64 log_moneyness = Math::log(forward / strike);

        // z = (nu / alpha) * f_avg * ln(F/K)
        f64 z = 0.0;
        if (alpha > 1e-15) {
            z = (nu / alpha) * f_avg * log_moneyness;
        }

        // x(z) = ln( (sqrt(1 - 2*rho*z + z^2) + z - rho) / (1 - rho) )
        f64 xz;
        f64 disc = 1.0 - 2.0 * rho * z + z * z;
        if (disc <= 0.0) disc = Limits<f64>::epsilon();

        f64 sqrt_disc = Math::sqrt(disc);
        f64 numer = sqrt_disc + z - rho;
        f64 denom = 1.0 - rho;
        if (numer <= 0.0) numer = Limits<f64>::epsilon();
        if (Math::abs(denom) < 1e-15) {
            xz = z;  // limit as rho -> 1
        } else {
            xz = Math::log(numer / denom);
        }

        // z / x(z) factor
        f64 z_over_xz;
        if (Math::abs(xz) > 1e-15) {
            z_over_xz = z / xz;
        } else {
            // Series expansion for small z:
            // z/x(z) = 1 + (rho/2)*z + ((1-3*rho^2)/12)*z^2 + ...
            z_over_xz = 1.0 + 0.5 * rho * z + (1.0 - 3.0 * rho * rho) * z * z / 12.0;
        }

        // Forward factor: alpha / f_avg
        f64 forward_factor = alpha / f_avg;

        // Base implied vol
        f64 sigma0 = forward_factor * z_over_xz;

        // Higher-order correction term:
        //
        // term1 = ((1-beta)^2 * alpha^2) / (24 * f_avg^2)
        // term2 = (rho * beta * nu * alpha) / (4 * f_avg)
        // term3 = ((2-3*rho^2) * nu^2) / 24
        //
        // corr = 1 + (term1 + term2 + term3) * T

        f64 f_avg_sq = f_avg * f_avg;
        f64 alpha_sq = alpha * alpha;
        f64 nu_sq = nu * nu;

        f64 term1 = ((1.0 - beta) * (1.0 - beta) * alpha_sq) / (24.0 * f_avg_sq);
        f64 term2 = (rho * beta * nu * alpha) / (4.0 * f_avg);
        f64 term3 = (2.0 - 3.0 * rho * rho) * nu_sq / 24.0;

        f64 corr = 1.0 + (term1 + term2 + term3) * expiry;

        f64 result = sigma0 * corr;

        // Clamp to reasonable range
        if (result < 1e-8) result = 1e-8;
        if (result > 10.0) result = 10.0;

        return result;
    }

    /// ATM SABR implied vol (when F = K):
    ///
    /// sigma_ATM = (alpha / F^(1-beta)) *
    ///   { 1 + [ ((1-beta)^2 * alpha^2) / (24 * F^(2-2*beta))
    ///         + (rho * beta * nu * alpha) / (4 * F^(1-beta))
    ///         + ((2-3*rho^2) * nu^2) / 24 ] * T }
    ///
    /// This is the limit as K -> F of the general formula.

    [[nodiscard]] static f64 implied_vol_atm(f64 forward, f64 expiry,
                                               f64 alpha, f64 beta,
                                               f64 rho, f64 nu) {
        if (forward <= 0.0 || expiry <= 0.0) return 0.0;
        f64 f_pow = Math::pow(forward, 1.0 - beta);
        if (f_pow < 1e-15) return 0.0;

        f64 alpha_over_f = alpha / f_pow;
        f64 alpha_sq = alpha * alpha;
        f64 nu_sq = nu * nu;
        f64 f_pow_sq = f_pow * f_pow;

        f64 term1 = ((1.0 - beta) * (1.0 - beta) * alpha_sq) / (24.0 * f_pow_sq);
        f64 term2 = (rho * beta * nu * alpha) / (4.0 * f_pow);
        f64 term3 = (2.0 - 3.0 * rho * rho) * nu_sq / 24.0;

        f64 result = alpha_over_f * (1.0 + (term1 + term2 + term3) * expiry);

        if (result < 1e-8) result = 1e-8;
        if (result > 10.0) result = 10.0;
        return result;
    }
};

// =========================================================================
// calibrate_sabr — calibrate SABR model to a volatility smile
// =========================================================================
//
// Calibrates alpha, rho, nu with beta fixed (user's choice of beta).
//
// Objective: minimize sum (sabr_vol(K_i) - market_vol_i)^2
//   optionally weighted by vega.

[[nodiscard]] inline CalibrationResult<SABRParams> calibrate_sabr(
    Slice<const f64> strikes,
    Slice<const f64> market_vols,
    f64 forward,
    f64 expiry,
    f64 beta = 1.0,
    f64 tolerance = 1e-6)
{
    CalibrationResult<SABRParams> result;
    u64 n = strikes.length();
    if (n < 3 || market_vols.length() != n) return result;

    // Initial guess based on market data heuristics
    // alpha ~ ATM vol * F^(1-beta)
    f64 f_pow = Math::pow(forward, 1.0 - beta);
    f64 atm_vol = market_vols[n / 2];  // approximate ATM
    f64 alpha0 = atm_vol * f_pow;
    f64 rho0 = -0.3;  // typical equity skew
    f64 nu0 = 0.3;    // moderate vol-of-vol

    SABRParams init;
    init.alpha_ = (alpha0 > 0.001) ? alpha0 : 0.2;
    init.beta_  = beta;
    init.rho_   = rho0;
    init.nu_    = nu0;

    // Bounds for parameters
    Vec<f64> lower = Vec<f64>::make(4);
    Vec<f64> upper = Vec<f64>::make(4);
    lower[0] = 1e-6;  upper[0] = 5.0;   // alpha
    lower[1] = beta;  upper[1] = beta;   // beta (fixed)
    lower[2] = -0.999; upper[2] = 0.999; // rho
    lower[3] = 0.0;    upper[3] = 5.0;   // nu

    // Objective function
    auto objective = [&](Slice<const f64> x) -> f64 {
        f64 a = x[0], b = x[1], r = x[2], v = x[3];
        f64 total = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 sabr_vol = SABRParams::implied_vol(forward, strikes[i],
                                                    expiry, a, b, r, v);
            f64 diff = sabr_vol - market_vols[i];
            total += diff * diff;
        }
        return Math::sqrt(total / static_cast<f64>(n));
    };

    // Use Calibrator::calibrate_obj
    // We need a Calibrator instance — but calibrate_obj is static, so just call it.
    // The Calibrator template requires Model and Params, but calibrate_obj on
    // Calibrator<void, SABRParams> works if we only use the static method.
    // Actually, calibrate_obj takes an auto objective, so we can call it directly.

    // Wrap: obj takes Slice<const f64>, CalibrationResult is generic on Params.
    // We'll run Nelder-Mead manually here for simplicity.

    Vec<f64> x0 = Vec<f64>::make(4);
    x0[0] = init.alpha_; x0[1] = init.beta_;
    x0[2] = init.rho_;   x0[3] = init.nu_;

    // ---- Nelder-Mead simplex optimization ----
    u64 n_params = 4;
    u64 m = n_params + 1;

    Vec<Vec<f64>> simplex;
    simplex.push(x0);
    for (u64 i = 0; i < n_params; i++) {
        Vec<f64> pt = x0.clone();
        f64 perturb = 0.05 * (upper[i] - lower[i]);
        if (perturb < 1e-6) perturb = 0.01;
        pt[i] += perturb;
        if (pt[i] > upper[i]) pt[i] = upper[i];
        simplex.push(spp::move(pt));
    }

    // Penalized objective for bounds
    auto bounded_obj = [&](Slice<const f64> x) -> f64 {
        f64 penalty = 0.0;
        for (u64 i = 0; i < n_params; i++) {
            if (x[i] < lower[i])
                penalty += (lower[i] - x[i]) * (lower[i] - x[i]) * 1e8;
            else if (x[i] > upper[i])
                penalty += (x[i] - upper[i]) * (x[i] - upper[i]) * 1e8;
        }
        return objective(x) + penalty;
    };

    Vec<f64> fvals = Vec<f64>::make(m);
    for (u64 i = 0; i < m; i++)
        fvals[i] = bounded_obj(simplex[i].slice());

    u64 max_iter = 500;
    for (u64 iter = 0; iter < max_iter; iter++) {
        // Sort simplex by f-value
        for (u64 i = 1; i < m; i++) {
            for (u64 j = i; j > 0; j--) {
                if (fvals[j] < fvals[j - 1]) {
                    f64 tf = fvals[j]; fvals[j] = fvals[j - 1]; fvals[j - 1] = tf;
                    Vec<f64> tv = spp::move(simplex[j]);
                    simplex[j] = spp::move(simplex[j - 1]);
                    simplex[j - 1] = spp::move(tv);
                } else break;
            }
        }

        result.error_history_.push(fvals[0]);
        result.function_evals_ += m;

        // Convergence check
        f64 fmean = 0.0;
        for (u64 i = 0; i < m; i++) fmean += fvals[i];
        fmean /= static_cast<f64>(m);
        f64 fstd = 0.0;
        for (u64 i = 0; i < m; i++) { f64 d = fvals[i] - fmean; fstd += d * d; }
        fstd = Math::sqrt(fstd / static_cast<f64>(m));
        if (fstd < tolerance) { result.converged_ = true; break; }

        // Centroid (exclude worst)
        Vec<f64> centroid = Vec<f64>::make(n_params);
        for (u64 i = 0; i < n_params; i++) centroid[i] = 0.0;
        for (u64 i = 0; i < m - 1; i++)
            for (u64 j = 0; j < n_params; j++) centroid[j] += simplex[i][j];
        for (u64 j = 0; j < n_params; j++) centroid[j] /= static_cast<f64>(m - 1);

        // Reflect
        Vec<f64> reflected = Vec<f64>::make(n_params);
        for (u64 j = 0; j < n_params; j++)
            reflected[j] = centroid[j] + 1.0 * (centroid[j] - simplex[m - 1][j]);
        f64 f_reflected = bounded_obj(reflected.slice());
        result.function_evals_++;

        if (f_reflected < fvals[0]) {
            // Expand
            Vec<f64> expanded = Vec<f64>::make(n_params);
            for (u64 j = 0; j < n_params; j++)
                expanded[j] = centroid[j] + 2.0 * (reflected[j] - centroid[j]);
            f64 f_expanded = bounded_obj(expanded.slice());
            result.function_evals_++;
            if (f_expanded < f_reflected) {
                simplex[m - 1] = spp::move(expanded);
                fvals[m - 1] = f_expanded;
            } else {
                simplex[m - 1] = spp::move(reflected);
                fvals[m - 1] = f_reflected;
            }
        } else if (f_reflected < fvals[m - 2]) {
            simplex[m - 1] = spp::move(reflected);
            fvals[m - 1] = f_reflected;
        } else {
            // Contract
            Vec<f64> contracted = Vec<f64>::make(n_params);
            if (f_reflected < fvals[m - 1]) {
                for (u64 j = 0; j < n_params; j++)
                    contracted[j] = centroid[j] + 0.5 * (reflected[j] - centroid[j]);
            } else {
                for (u64 j = 0; j < n_params; j++)
                    contracted[j] = centroid[j] + 0.5 * (simplex[m - 1][j] - centroid[j]);
            }
            f64 f_contracted = bounded_obj(contracted.slice());
            result.function_evals_++;
            if (f_contracted < Math::min(f_reflected, fvals[m - 1])) {
                simplex[m - 1] = spp::move(contracted);
                fvals[m - 1] = f_contracted;
            } else {
                // Shrink
                for (u64 i = 1; i < m; i++) {
                    for (u64 j = 0; j < n_params; j++)
                        simplex[i][j] = simplex[0][j] + 0.5 * (simplex[i][j] - simplex[0][j]);
                    fvals[i] = bounded_obj(simplex[i].slice());
                    result.function_evals_++;
                }
            }
        }
        result.iterations_ = iter + 1;
    }

    Slice<const f64> best = simplex[0].slice();
    result.params_.alpha_ = best[0];
    result.params_.beta_  = best[1];
    result.params_.rho_   = best[2];
    result.params_.nu_    = best[3];
    result.error_ = fvals[0];
    return result;
}

} // namespace spp::quant::calib
