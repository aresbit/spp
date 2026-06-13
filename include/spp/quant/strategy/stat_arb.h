#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/strategy/strategy.h"
#include "spp/quant/math/statistics.h"
#include "spp/quant/math/distributions.h"

namespace spp::quant {

// =========================================================================
// PairsTrade — a cointegrated pair with trading signals
// =========================================================================
//
// Models the relationship: asset_a = alpha + hedge_ratio * asset_b + spread
// where the spread is mean-reverting (stationary).
//
// Trading rule:
//   - Long spread (buy A, sell B * hedge_ratio) when z < -entry_z
//   - Short spread (sell A, buy B * hedge_ratio) when z > +entry_z
//   - Close when |z| < exit_z
//
// Uses unified SignalDirection: Long / Short / Flat.

struct PairsTrade {
    String_View asset_a_;
    String_View asset_b_;
    f64 hedge_ratio_  = 1.0;   ///< beta from A = alpha + beta * B
    f64 spread_mean_  = 0.0;   ///< mean of spread = A - hedge_ratio * B
    f64 spread_std_   = 1.0;   ///< standard deviation of spread
    f64 half_life_    = 5.0;   ///< mean-reversion half-life (bars)

    // Cointegration test statistics
    f64 adf_statistic_ = 0.0;  ///< ADF test statistic on the spread residuals
    f64 adf_pvalue_    = 1.0;  ///< approximate p-value

    // Current state
    f64 current_spread_  = 0.0;
    f64 current_z_score_ = 0.0;

    SPP_RECORD(PairsTrade, SPP_FIELD(asset_a_), SPP_FIELD(asset_b_),
               SPP_FIELD(hedge_ratio_), SPP_FIELD(spread_mean_),
               SPP_FIELD(spread_std_));

    /// Generate a trading signal from the current spread z-score.
    ///
    /// entry_z: enter when |z| > entry_z (default 2.0 = 95% CI under normality)
    /// exit_z:  exit  when |z| < exit_z  (default 0.5)
    ///
    /// For the pair (A, B):
    ///   z >  entry_z: A is expensive relative to B -> sell A, buy B
    ///   z < -entry_z: A is cheap relative to B    -> buy A, sell B

    [[nodiscard]] Opt<SignalEvent> generate_signal(f64 entry_z = 2.0,
                                                     f64 exit_z = 0.5) const {
        f64 z = current_z_score_;
        SignalEvent sig;
        sig.date_ = Date::today();

        if (z > entry_z) {
            // A is overvalued relative to B: short A, long B
            sig.symbol_    = asset_a_;
            sig.direction_ = SignalDirection::Short;
            sig.strength_  = -Math::min(1.0, (z - entry_z) / entry_z);
            return Opt{spp::move(sig)};
        } else if (z < -entry_z) {
            // A is undervalued relative to B: long A, short B
            sig.symbol_    = asset_a_;
            sig.direction_ = SignalDirection::Long;
            sig.strength_  = Math::min(1.0, (-z - entry_z) / entry_z);
            return Opt{spp::move(sig)};
        } else if (Math::abs(z) < exit_z) {
            // Spread has reverted: exit
            sig.symbol_    = asset_a_;
            sig.direction_ = SignalDirection::Flat;
            sig.strength_  = 0.0;
            return Opt{spp::move(sig)};
        }

        return {};
    }

    /// Update the current spread state given new prices for A and B
    void update_spread(f64 price_a, f64 price_b) {
        current_spread_ = price_a - hedge_ratio_ * price_b;
        if (spread_std_ > 1e-15)
            current_z_score_ = (current_spread_ - spread_mean_) / spread_std_;
        else
            current_z_score_ = 0.0;
    }
};

// =========================================================================
// ADFTest — Augmented Dickey-Fuller test for stationarity
// =========================================================================
//
// Tests H0: series has a unit root (non-stationary)
//      H1: series is stationary
//
// Regression: Delta_y_t = alpha + beta * t + gamma * y_{t-1}
//                        + sum_{i=1}^{p} delta_i * Delta_y_{t-i} + eps_t
//
// Test statistic: tau = gamma_hat / SE(gamma_hat)
// Reject H0 if tau < critical_value (i.e., sufficiently negative)

struct ADFTest {
    u64 max_lags_ = 10;  ///< maximum number of lagged differences to include

    struct Result {
        f64  statistic_;
        f64  p_value_;        ///< approximate from MacKinnon (1994) response surfaces
        u64  lags_used_;
        bool is_stationary_;  ///< true if H0 rejected at 5% significance
    };

    /// Run the ADF test on a time series.
    /// Uses automatic lag selection: choose the lag that minimizes AIC/BIC
    /// (Schwarz criterion), up to max_lags_.

    [[nodiscard]] Result test(Slice<const f64> series) const {
        Result result;
        u64 n = series.length();
        if (n < 10) {
            result.is_stationary_ = false;
            return result;
        }

        // Build lagged difference matrix and find best lag via BIC
        u64 best_lag = 0;
        f64 best_bic = Limits<f64>::max();
        f64 best_stat = 0.0;
        u64 max_lag = Math::min(max_lags_, n / 4);

        for (u64 p = 0; p <= max_lag; p++) {
            f64 stat = compute_adf_statistic(series, p);
            f64 bic = compute_bic(series, p, stat);
            if (bic < best_bic) {
                best_bic = bic;
                best_lag = p;
                best_stat = stat;
            }
        }

        result.statistic_ = best_stat;
        result.lags_used_ = best_lag;
        result.p_value_ = approximate_pvalue(best_stat, n);
        result.is_stationary_ = (best_stat < critical_value(n, 0.05));

        return result;
    }

    /// Compute the ADF t-statistic for a given lag order p.
    /// Regression: Delta_y_t = alpha + gamma * y_{t-1} + sum_{i=1}^{p} delta_i * Delta_y_{t-i}
    /// Returns t-stat = gamma_hat / SE(gamma_hat)

    [[nodiscard]] static f64 compute_adf_statistic(Slice<const f64> y, u64 p) {
        u64 n = y.length();
        u64 nobs = n - p - 1;  // effective observations after lags
        if (nobs < 5) return 0.0;

        // Build regressor matrix X (nobs rows, 2 + p cols):
        // Col 0: constant (1.0)
        // Col 1: y_{t-1} (lagged level)
        // Cols 2..2+p-1: Delta_y_{t-i} for i=1..p

        u64 ncols = 2 + p;  // constant + y_{t-1} + p lagged diffs

        // Compute X^T * X (symmetric) and X^T * y_vec
        // For small p (typically <= 10), a direct solve is efficient.

        // First, build vectors for each column of X
        Vec<f64> col0 = Vec<f64>::make(nobs);  // constant
        Vec<f64> col1 = Vec<f64>::make(nobs);  // y_{t-1}
        Vec<Vec<f64>> lag_cols;
        for (u64 j = 0; j < p; j++)
            lag_cols.push(Vec<f64>::make(nobs));

        Vec<f64> dy_vec = Vec<f64>::make(nobs);  // Delta_y_t

        // Observing y[0], y[1], ..., y[n-1]
        // For t = p, p+1, ..., n-1:
        //   LHS: Delta_y_t = y[t] - y[t-1]
        //   RHS: intercept + gamma * y[t-1] + sum_{i=1}^{p} delta_i * (y[t-i] - y[t-i-1])

        for (u64 t = p; t < n; t++) {
            u64 row = t - p;
            col0[row] = 1.0;
            col1[row] = y[t - 1];                    // y_{t-1}
            for (u64 i = 0; i < p; i++) {
                u64 lag_ = i + 1;
                lag_cols[i][row] = y[t - lag_] - y[t - lag_ - 1];  // Delta_y_{t-i}
            }
            dy_vec[row] = y[t] - y[t - 1];           // Delta_y_t
        }

        // Now solve: (X^T X) beta = X^T dy
        // Use normal equations with Cholesky (ncols is small)

        // Build X^T X and X^T dy
        Vec<f64> XtX = Vec<f64>::make(ncols * ncols);
        Vec<f64> Xty = Vec<f64>::make(ncols);

        // Helper: dot product of two column vectors
        auto dot = [](Slice<const f64> a, Slice<const f64> b) -> f64 {
            f64 s = 0.0;
            u64 len = a.length();
            for (u64 i = 0; i < len; i++) s += a[i] * b[i];
            return s;
        };

        // Build XtX as a flat row-major matrix
        // Col indices: 0 = constant, 1 = y_{t-1}, 2..2+p-1 = lagged diffs
        auto col_ptr = [&](u64 j) -> Slice<const f64> {
            if (j == 0) return col0.slice();
            if (j == 1) return col1.slice();
            return lag_cols[j - 2].slice();
        };

        for (u64 j = 0; j < ncols; j++) {
            Xty[j] = dot(col_ptr(j), dy_vec.slice());
            for (u64 k = j; k < ncols; k++) {
                f64 val = dot(col_ptr(j), col_ptr(k));
                XtX[j * ncols + k] = val;
                XtX[k * ncols + j] = val;  // symmetric
            }
        }

        // Solve using Cholesky decomposition on XtX
        // Cholesky: L * L^T = XtX
        // Then: L * y = Xty, L^T * beta = y
        Vec<f64> L = Vec<f64>::make(ncols * ncols);
        for (u64 i = 0; i < ncols; i++) {
            for (u64 j = 0; j <= i; j++) {
                f64 sum = 0.0;
                for (u64 k = 0; k < j; k++)
                    sum += L[i * ncols + k] * L[j * ncols + k];
                if (i == j) {
                    f64 diag = XtX[i * ncols + i] - sum;
                    if (diag <= 0.0) return 0.0;  // not positive definite
                    L[i * ncols + j] = Math::sqrt(diag);
                } else {
                    L[i * ncols + j] = (XtX[i * ncols + j] - sum) / L[j * ncols + j];
                }
            }
        }

        // Forward substitution: L * y = Xty
        Vec<f64> y_sol = Vec<f64>::make(ncols);
        for (u64 i = 0; i < ncols; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < i; j++)
                sum += L[i * ncols + j] * y_sol[j];
            y_sol[i] = (Xty[i] - sum) / L[i * ncols + i];
        }

        // Backward substitution: L^T * beta = y_sol
        Vec<f64> beta = Vec<f64>::make(ncols);
        for (u64 i_i = ncols; i_i > 0; i_i--) {
            u64 i = i_i - 1;
            f64 sum = 0.0;
            for (u64 j = i + 1; j < ncols; j++)
                sum += L[j * ncols + i] * beta[j];
            beta[i] = (y_sol[i] - sum) / L[i * ncols + i];
        }

        f64 gamma_hat = beta[1];  // coefficient on y_{t-1}

        // Compute residuals and estimate standard error
        Vec<f64> residuals = Vec<f64>::make(nobs);
        f64 rss = 0.0;
        for (u64 t = 0; t < nobs; t++) {
            f64 pred = beta[0];  // intercept
            pred += gamma_hat * col1[t];
            for (u64 j = 0; j < p; j++)
                pred += beta[2 + j] * lag_cols[j][t];
            residuals[t] = dy_vec[t] - pred;
            rss += residuals[t] * residuals[t];
        }

        u64 df_resid = nobs - ncols;
        if (df_resid == 0) return 0.0;
        f64 sigma2 = rss / static_cast<f64>(df_resid);

        // SE(gamma_hat) = sigma * sqrt(diag(X^T X)^{-1}[1,1])
        // Compute the (1,1) element of the inverse via forward/backward substitution

        // Solve X^T X * [a, b] = [0, 1, 0, ...]  to get col 1 of inverse
        // L * L^T * inv_col = e1
        // Forward: L * w = e1
        Vec<f64> e1 = Vec<f64>::make(ncols);
        e1[1] = 1.0;  // unit vector for col 1 (gamma_hat position)
        Vec<f64> w = Vec<f64>::make(ncols);
        for (u64 i = 0; i < ncols; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < i; j++)
                sum += L[i * ncols + j] * w[j];
            w[i] = (e1[i] - sum) / L[i * ncols + i];
        }
        // Backward: L^T * inv_col = w
        Vec<f64> inv_col1 = Vec<f64>::make(ncols);
        for (u64 i_i = ncols; i_i > 0; i_i--) {
            u64 i = i_i - 1;
            f64 sum = 0.0;
            for (u64 j = i + 1; j < ncols; j++)
                sum += L[j * ncols + i] * inv_col1[j];
            inv_col1[i] = (w[i] - sum) / L[i * ncols + i];
        }
        f64 var_gamma = sigma2 * inv_col1[1];  // diagonal element at position 1
        if (var_gamma <= 0.0) return 0.0;

        f64 se_gamma = Math::sqrt(var_gamma);
        if (se_gamma < 1e-15) return 0.0;

        return gamma_hat / se_gamma;  // t-statistic
    }

    /// Bayesian Information Criterion for lag selection:
    /// BIC = n * ln(RSS/n) + k * ln(n)
    /// where k = number of parameters (2 + p), n = effective sample size.

    [[nodiscard]] static f64 compute_bic(Slice<const f64> y, u64 p, f64 /*stat*/) {
        u64 n = y.length();
        u64 nobs = n - p - 1;
        if (nobs < 5) return Limits<f64>::max();

        // Compute RSS for this lag order (simplified: just use the regression)
        u64 ncols = 2 + p;
        Vec<f64> dy_vec = Vec<f64>::make(nobs);
        Vec<f64> x0 = Vec<f64>::make(nobs);
        Vec<f64> x1 = Vec<f64>::make(nobs);

        for (u64 t = p; t < n; t++) {
            u64 row = t - p;
            x0[row] = 1.0;
            x1[row] = y[t - 1];
            dy_vec[row] = y[t] - y[t - 1];
        }

        // Build X^T X and solve as before.
        Vec<f64> XtX = Vec<f64>::make(ncols * ncols);
        Vec<f64> Xty = Vec<f64>::make(ncols);

        for (u64 j = 0; j < ncols; j++) {
            for (u64 t = 0; t < nobs; t++) {
                f64 val_j;
                if (j == 0) val_j = 1.0;
                else if (j == 1) val_j = y[t + p - 1];
                else {
                    u64 lag_ = j - 1;
                    val_j = y[t + p - lag_] - y[t + p - lag_ - 1];
                }
                Xty[j] += val_j * dy_vec[t];
                for (u64 k = j; k < ncols; k++) {
                    f64 val_k;
                    if (k == 0) val_k = 1.0;
                    else if (k == 1) val_k = y[t + p - 1];
                    else {
                        u64 lag_ = k - 1;
                        val_k = y[t + p - lag_] - y[t + p - lag_ - 1];
                    }
                    f64 prod = val_j * val_k;
                    XtX[j * ncols + k] += prod;
                    if (j != k) XtX[k * ncols + j] += prod;
                }
            }
        }

        // Solve and compute RSS
        Vec<f64> beta = solve_small_system(XtX, Xty, ncols);
        f64 rss = 0.0;
        for (u64 t = 0; t < nobs; t++) {
            f64 pred = 0.0;
            for (u64 j = 0; j < ncols; j++) {
                f64 val_j;
                if (j == 0) val_j = 1.0;
                else if (j == 1) val_j = y[t + p - 1];
                else {
                    u64 lag_ = j - 1;
                    val_j = y[t + p - lag_] - y[t + p - lag_ - 1];
                }
                pred += beta[j] * val_j;
            }
            f64 err = dy_vec[t] - pred;
            rss += err * err;
        }

        if (rss <= 0.0) return Limits<f64>::max();

        f64 k = static_cast<f64>(ncols);
        return static_cast<f64>(nobs) * Math::log(rss / static_cast<f64>(nobs))
             + k * Math::log(static_cast<f64>(nobs));
    }

    /// Solve small linear system using Cholesky (internal helper)
    [[nodiscard]] static Vec<f64> solve_small_system(Slice<const f64> XtX_flat,
                                                      Slice<const f64> Xty,
                                                      u64 ncols) {
        Vec<f64> beta = Vec<f64>::make(ncols);

        // Copy XtX to mutable L matrix
        Vec<f64> L = Vec<f64>::make(ncols * ncols);
        for (u64 i = 0; i < ncols * ncols; i++) L[i] = XtX_flat[i];

        // Cholesky in-place
        for (u64 i = 0; i < ncols; i++) {
            for (u64 j = 0; j <= i; j++) {
                f64 sum = 0.0;
                for (u64 k = 0; k < j; k++)
                    sum += L[i * ncols + k] * L[j * ncols + k];
                if (i == j) {
                    f64 diag = L[i * ncols + i] - sum;
                    if (diag <= 0.0) {
                        // Not positive definite; return zeros
                        for (u64 z = 0; z < ncols; z++) beta[z] = 0.0;
                        return beta;
                    }
                    L[i * ncols + j] = Math::sqrt(diag);
                } else {
                    L[i * ncols + j] = (L[i * ncols + j] - sum) / L[j * ncols + j];
                }
            }
        }

        // Forward substitution
        Vec<f64> y = Vec<f64>::make(ncols);
        for (u64 i = 0; i < ncols; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < i; j++)
                sum += L[i * ncols + j] * y[j];
            y[i] = (Xty[i] - sum) / L[i * ncols + i];
        }

        // Back substitution
        for (u64 i_i = ncols; i_i > 0; i_i--) {
            u64 i = i_i - 1;
            f64 sum = 0.0;
            for (u64 j = i + 1; j < ncols; j++)
                sum += L[j * ncols + i] * beta[j];
            beta[i] = (y[i] - sum) / L[i * ncols + i];
        }

        return beta;
    }

    // =====================================================================
    // MacKinnon (1994) critical values for the ADF test
    // =====================================================================
    //
    // Approximate critical values using response surfaces from
    // MacKinnon, J.G. (1994) "Approximate asymptotic distribution functions
    // for unit-root and cointegration tests."
    //
    // The response surface is:
    //   CV(p) = beta_inf + beta1/T + beta2/T^2
    //
    // where T = sample size.

    [[nodiscard]] static f64 critical_value(u64 n, f64 significance = 0.05) {
        // MacKinnon (1994) response surface parameters for ADF test
        // (constant, no trend case)
        // Table 1 in MacKinnon (1994)

        f64 beta_inf, beta1, beta2;

        if (significance <= 0.01) {
            beta_inf = -3.4335;
            beta1   = -5.999;
            beta2   = -29.25;
        } else if (significance <= 0.05) {
            beta_inf = -2.8621;
            beta1   = -2.738;
            beta2   = -8.36;
        } else {
            beta_inf = -2.5671;
            beta1   = -1.438;
            beta2   = -4.48;
        }

        f64 T = static_cast<f64>(n);
        return beta_inf + beta1 / T + beta2 / (T * T);
    }

    /// Approximate p-value from the ADF test statistic using
    /// MacKinnon's (1994) response surface for p-values.
    ///
    /// This is an approximate mapping; for exact p-values, use the
    /// full response surface tables.

    [[nodiscard]] static f64 approximate_pvalue(f64 stat, u64 n) {
        // Use linear interpolation between critical values to approximate p-value.
        f64 cv01 = critical_value(n, 0.01);
        f64 cv05 = critical_value(n, 0.05);
        f64 cv10 = critical_value(n, 0.10);

        if (stat <= cv01) return 0.001;

        if (stat <= cv05) {
            f64 frac = (stat - cv01) / (cv05 - cv01);
            return 0.01 + frac * 0.04;
        }

        if (stat <= cv10) {
            f64 frac = (stat - cv05) / (cv10 - cv05);
            return 0.05 + frac * 0.05;
        }

        return 0.10 + (stat - cv10) / (-cv10) * 0.40;
    }
};

// =========================================================================
// PairFinder — find cointegrated pairs from a set of asset time series
// =========================================================================

struct PairFinder {
    /// Find the best cointegrated pairs from a universe of assets.
    ///
    /// Algorithm:
    ///   1. Compute all pairwise Pearson correlations.
    ///   2. Filter to pairs with correlation >= min_correlation.
    ///   3. For each surviving pair, run the Engle-Granger two-step:
    ///      a. Regress Y on X to get hedge ratio and residuals.
    ///      b. Test residuals for stationarity using ADF test.
    ///   4. Filter by half-life: min_half_life <= half_life <= max_half_life.
    ///   5. Rank by ADF statistic (most negative = most stationary spread).
    ///   6. Return top_n pairs.

    [[nodiscard]] Vec<PairsTrade> find_pairs(
        const Map<String_View, Slice<const f64>>& asset_prices,
        u64 top_n = 10,
        f64 min_correlation = 0.7,
        f64 min_half_life = 1.0,
        f64 max_half_life = 50.0) const
    {
        Vec<PairsTrade> results;

        // Collect asset names and data into vectors for pairwise access
        u64 num_assets = asset_prices.size();
        if (num_assets < 2) return results;

        Vec<String_View> names;
        Vec<Slice<const f64>> price_slices;
        for (const auto& kv : asset_prices) {
            names.push(kv.first);
            price_slices.push(kv.second);
        }

        // Pairwise scan
        for (u64 i = 0; i < num_assets; i++) {
            for (u64 j = i + 1; j < num_assets; j++) {
                // Compute correlation
                f64 corr = stat::correlation(price_slices[i], price_slices[j]);
                if (Math::abs(corr) < min_correlation) continue;

                // Test cointegration (A on B)
                auto pair_a = test_pair(names[i], names[j],
                                         price_slices[i], price_slices[j]);
                if (pair_a.ok()) {
                    if (pair_a->half_life_ >= min_half_life
                        && pair_a->half_life_ <= max_half_life) {
                        results.push(spp::move(*pair_a));
                    }
                }

                // Test cointegration (B on A) — reverse direction
                auto pair_b = test_pair(names[j], names[i],
                                         price_slices[j], price_slices[i]);
                if (pair_b.ok()) {
                    if (pair_b->half_life_ >= min_half_life
                        && pair_b->half_life_ <= max_half_life) {
                        results.push(spp::move(*pair_b));
                    }
                }
            }
        }

        // Sort by ADF statistic (most negative first = most stationary)
        // Simple insertion sort
        u64 m = results.length();
        for (u64 i = 1; i < m; i++) {
            PairsTrade key = spp::move(results[i]);
            i64 j = static_cast<i64>(i) - 1;
            while (j >= 0 && results[static_cast<u64>(j)].adf_statistic_ > key.adf_statistic_) {
                results[static_cast<u64>(j + 1)] = spp::move(results[static_cast<u64>(j)]);
                j--;
            }
            results[static_cast<u64>(j + 1)] = spp::move(key);
        }

        // Truncate to top_n
        if (results.length() > top_n) {
            Vec<PairsTrade> truncated;
            for (u64 i = 0; i < top_n; i++)
                truncated.push(spp::move(results[i]));
            results = spp::move(truncated);
        }

        return results;
    }

    /// Engle-Granger two-step cointegration test for a single pair.
    ///
    /// Step 1: Regress Y on X to get hedge ratio:
    ///         Y_t = alpha + beta * X_t + eps_t
    /// Step 2: Test residuals eps_t for stationarity (ADF test).
    ///
    /// Returns PairsTrade if the spread is stationary (cointegrated),
    /// otherwise returns empty Opt.

    [[nodiscard]] Opt<PairsTrade> test_pair(String_View a, String_View b,
                                              Slice<const f64> prices_a,
                                              Slice<const f64> prices_b) const {
        u64 n = Math::min(prices_a.length(), prices_b.length());
        if (n < 20) return {};

        // Step 1: OLS regression: A = alpha + beta * B
        stat::OLS_Result ols = stat::ols_regression(prices_b, prices_a);

        // Compute spread residuals: spread_t = A_t - alpha - beta * B_t
        Vec<f64> spread = Vec<f64>::make(n);
        for (u64 t = 0; t < n; t++) {
            spread[t] = prices_a[t] - ols.alpha - ols.beta * prices_b[t];
        }

        // Step 2: ADF test on the spread residuals
        ADFTest adf;
        ADFTest::Result adf_result = adf.test(spread.slice());

        // Only return pairs where spread is stationary
        if (!adf_result.is_stationary_) return {};

        // Compute spread statistics
        f64 spread_mean = 0.0;
        f64 spread_var = 0.0;
        for (u64 t = 0; t < n; t++) {
            spread_mean += spread[t];
        }
        spread_mean /= static_cast<f64>(n);
        for (u64 t = 0; t < n; t++) {
            f64 diff = spread[t] - spread_mean;
            spread_var += diff * diff;
        }
        spread_var /= static_cast<f64>(n - 1);
        f64 spread_std = Math::sqrt(spread_var);

        // Compute half-life of mean reversion
        // For an AR(1) process: spread_t = rho * spread_{t-1} + eps_t
        // half_life = -ln(2) / ln(|rho|)
        stat::OLS_Result ar1 = stat::ols_regression(
            spread.slice().sub(0, n - 1), spread.slice().sub(1, n - 1));
        f64 rho = ar1.beta;
        f64 half_life = 5.0;  // default
        if (rho > 0.0 && rho < 1.0) {
            half_life = -Math::log(2.0) / Math::log(rho);
        } else if (rho <= 0.0) {
            half_life = 1.0;  // instant mean reversion
        } else {
            half_life = 50.0; // effectively non-stationary
        }

        PairsTrade pt;
        pt.asset_a_        = a;
        pt.asset_b_        = b;
        pt.hedge_ratio_    = ols.beta;
        pt.spread_mean_    = spread_mean;
        pt.spread_std_     = spread_std;
        pt.half_life_      = half_life;
        pt.adf_statistic_  = adf_result.statistic_;
        pt.adf_pvalue_     = adf_result.pvalue_;
        pt.current_spread_ = spread[n - 1];
        pt.current_z_score_ = (spread_std > 1e-15)
            ? (spread[n - 1] - spread_mean) / spread_std : 0.0;

        return Opt{spp::move(pt)};
    }
};

// =========================================================================
// MarketMaking — Avellaneda-Stoikov (2008) market making model
// =========================================================================
//
// Optimal bid and ask quotes given inventory risk aversion.
//
// Reservation (indifference) price:
//   r(s, q, t) = s - q * gamma * sigma^2 * (T - t)
//
// where:
//   s     = current mid price
//   q     = current inventory (positive = long)
//   gamma = inventory risk aversion parameter
//   sigma = asset volatility
//   T - t = time remaining in trading session
//
// Optimal spread around reservation price:
//   delta_a = r + gamma * sigma^2 * (T - t) / 2 + (1/gamma) * ln(1 + gamma / k)
//   delta_b = r - gamma * sigma^2 * (T - t) / 2 - (1/gamma) * ln(1 + gamma / k)
//
// where k is the order arrival intensity parameter.
//
// Bid = r - spread/2, Ask = r + spread/2
// where spread = delta_a - delta_b = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)

struct MarketMaking : Strategy {
    // A-S model parameters
    f64 gamma_         = 0.1;    ///< inventory risk aversion
    f64 sigma_         = 0.2;    ///< annualized volatility
    f64 T_             = 1.0;    ///< time horizon (fraction of year, e.g. 1/252 = 1 day)
    f64 k_             = 1.5;    ///< order arrival intensity
    f64 inventory_     = 0.0;    ///< current inventory
    f64 max_inventory_ = 100.0;  ///< maximum absolute inventory
    f64 mid_price_     = 0.0;    ///< current mid price
    f64 time_remaining_= 1.0;    ///< current time remaining (T - t)

    // Computed quotes
    f64 reservation_price_ = 0.0;
    f64 optimal_spread_    = 0.0;

    // =====================================================================
    // Update reservation price and optimal spread based on current state.
    //
    // Reservation price: r = s - q * gamma * sigma^2 * (T - t)
    //
    // The intuition: if you are long (q > 0), your reservation price
    // is below the mid — you are willing to sell at a discount to
    // reduce inventory risk. If short (q < 0), you are willing to
    // buy at a premium.
    //
    // Optimal spread:
    //   spread = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
    //
    // Component 1: gamma * sigma^2 * (T - t) — compensation for
    //   adverse selection over the remaining horizon.
    // Component 2: (2/gamma)*ln(1+gamma/k) — compensation for
    //   inventory risk from the bid-ask bounce.
    // =====================================================================

    void update_model(f64 mid_price, f64 time_remaining) {
        mid_price_      = mid_price;
        time_remaining_ = time_remaining;

        f64 tau = time_remaining_;  // T - t
        if (tau < 0.0) tau = 0.0;

        // Reservation price
        reservation_price_ = mid_price - inventory_ * gamma_ * sigma_ * sigma_ * tau;

        // Optimal spread
        f64 adverse_selection_term = gamma_ * sigma_ * sigma_ * tau;
        f64 inventory_risk_term;
        if (gamma_ > 1e-15 && k_ > 1e-15) {
            f64 ratio = 1.0 + gamma_ / k_;
            if (ratio > 0.0)
                inventory_risk_term = (2.0 / gamma_) * Math::log(ratio);
            else
                inventory_risk_term = 0.0;
        } else {
            inventory_risk_term = 0.0;
        }
        optimal_spread_ = adverse_selection_term + inventory_risk_term;

        // Clamp spread to be non-negative and reasonable
        if (optimal_spread_ < 0.0) optimal_spread_ = 0.0;
        if (optimal_spread_ > mid_price * 0.5) optimal_spread_ = mid_price * 0.5;
    }

    /// Bid price: reservation price minus half the spread
    [[nodiscard]] f64 bid_price() const {
        return reservation_price_ - 0.5 * optimal_spread_;
    }

    /// Ask price: reservation price plus half the spread
    [[nodiscard]] f64 ask_price() const {
        return reservation_price_ + 0.5 * optimal_spread_;
    }

    /// Bid size: reduce when inventory is long, increase when short
    [[nodiscard]] f64 bid_size() const {
        f64 base = (optimal_spread_ > 1e-15) ? 1.0 / optimal_spread_ : 10.0;
        f64 skew = 1.0 - (inventory_ / max_inventory_);
        if (skew < 0.0) skew = 0.0;
        if (skew > 2.0) skew = 2.0;
        return base * skew;
    }

    /// Ask size: reduce when inventory is short, increase when long
    [[nodiscard]] f64 ask_size() const {
        f64 base = (optimal_spread_ > 1e-15) ? 1.0 / optimal_spread_ : 10.0;
        f64 skew = 1.0 + (inventory_ / max_inventory_);
        if (skew < 0.0) skew = 0.0;
        if (skew > 2.0) skew = 2.0;
        return base * skew;
    }

    // ---- Strategy interface implementation ----

    Opt<SignalEvent> on_market_data(const MarketEvent& event) override {
        // Update the A-S model with each new price
        mid_price_ = event.close_;
        // Assume constant time step of 1/252 per bar (daily)
        time_remaining_ -= 1.0 / 252.0;
        update_model(mid_price_, time_remaining_);

        // Market making typically fires signals continuously,
        // not just at extremes. The on_signal handler is where
        // quotes are generated.
        return {};
    }

    Vec<OrderEvent> on_signal(const SignalEvent& signal) override {
        // Generate quotes as limit orders.
        Vec<OrderEvent> orders;

        // If inventory is at the limit, only quote to reduce position
        if (inventory_ <= -max_inventory_) {
            // Too short: only place buy orders
            OrderEvent buy;
            buy.symbol_   = signal.symbol_;
            buy.side_     = OrderSide::Buy;
            buy.price_    = bid_price();
            buy.quantity_ = bid_size();
            buy.date_     = signal.date_;
            orders.push(spp::move(buy));
            return orders;
        }

        if (inventory_ >= max_inventory_) {
            // Too long: only place sell orders
            OrderEvent sell;
            sell.symbol_   = signal.symbol_;
            sell.side_     = OrderSide::Sell;
            sell.price_    = ask_price();
            sell.quantity_ = ask_size();
            sell.date_     = signal.date_;
            orders.push(spp::move(sell));
            return orders;
        }

        // Normal quoting: place both bid and ask
        {
            OrderEvent bid;
            bid.symbol_   = signal.symbol_;
            bid.side_     = OrderSide::Buy;
            bid.price_    = bid_price();
            bid.quantity_ = bid_size();
            bid.date_     = signal.date_;
            orders.push(spp::move(bid));
        }
        {
            OrderEvent ask;
            ask.symbol_   = signal.symbol_;
            ask.side_     = OrderSide::Sell;
            ask.price_    = ask_price();
            ask.quantity_ = ask_size();
            ask.date_     = signal.date_;
            orders.push(spp::move(ask));
        }

        return orders;
    }

    void on_fill(const FillEvent& fill) override {
        f64 signed_qty = fill.signed_quantity();
        inventory_ += signed_qty;

        f64 cost = fill.price_ * fill.quantity_ + fill.commission_;
        if (fill.side_ == OrderSide::Buy)
            cash_ -= cost;
        else
            cash_ += fill.price_ * fill.quantity_ - fill.commission_;

        // Update position book
        auto pos_opt = positions_.find(fill.symbol_);
        if (pos_opt.ok()) {
            Position& pos = **pos_opt;
            if (pos.quantity_ + signed_qty == 0.0) {
                positions_.remove(fill.symbol_);
            } else {
                f64 new_qty = pos.quantity_ + signed_qty;
                f64 new_cost_basis = pos.quantity_ * pos.entry_price_ + signed_qty * fill.price_;
                pos.quantity_ = new_qty;
                pos.entry_price_ = (new_qty != 0.0) ? new_cost_basis / new_qty : 0.0;
            }
        } else if (signed_qty != 0.0) {
            Position new_pos;
            new_pos.instrument_id_ = fill.symbol_;
            new_pos.quantity_      = signed_qty;
            new_pos.entry_price_   = fill.price_;
            new_pos.entry_date_    = fill.date_;
            positions_.add(spp::move(new_pos));
        }
    }
};

} // namespace spp::quant
