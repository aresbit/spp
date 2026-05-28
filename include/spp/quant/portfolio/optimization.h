#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/math/matrix.h"

namespace spp::quant {

// =========================================================================
// OptimizationResult — output of portfolio optimization
// =========================================================================
struct OptimizationResult {
    Vec<f64> optimal_weights_;
    f64      expected_return_     = 0.0;
    f64      expected_volatility_ = 0.0;
    f64      sharpe_ratio_        = 0.0;
    bool     optimal_             = false;

    SPP_RECORD(OptimizationResult, SPP_FIELD(optimal_weights_),
               SPP_FIELD(expected_return_), SPP_FIELD(expected_volatility_),
               SPP_FIELD(sharpe_ratio_), SPP_FIELD(optimal_));
};

// =========================================================================
// markowitz — unconstrained mean-variance optimization (analytical solution)
//
// Minimize:   w^T * cov * w
// Subject to: sum(w) = 1
//             w^T * mu >= target_return (if provided)
//
// For the unconstrained case (sum-to-1 only), use the analytical tangency /
// minimum-variance solution.  When a target_return is specified, use the
// two-fund separation theorem:
//
//   w* = w_g + lambda * w_d
//
// where w_g is the global minimum variance portfolio,
//       w_d is the diversification component,
// and lambda is chosen to hit the target return.
//
// Notation:
//   a = 1^T * inv_cov * 1
//   b = 1^T * inv_cov * mu
//   c = mu^T * inv_cov * mu
//   delta = a * c - b^2
//
// GMV weights:      w_g = (1/a) * inv_cov * 1
// Tangency weights: w_t = (1/b) * inv_cov * mu  (up to scaling)
//
// Target return lambda:
//   lambda = (target_return - b/a) / (delta / a)
//   w* = w_g + lambda * (w_t - w_g) * (b/a) … or equivalently
//   w* = (c - target_return*b)/delta * inv_cov * 1
//      + (target_return*a - b)/delta * inv_cov * mu
// =========================================================================
inline OptimizationResult markowitz(
    Slice<const f64> expected_returns,
    const linalg::Matrix<>& covariance,
    f64 risk_free_rate = 0.0,
    Opt<f64> target_return = {}) noexcept {

    u64 n = expected_returns.length();
    OptimizationResult result;
    result.optimal_ = false;

    if (n == 0 || covariance.rows_ != n || covariance.cols_ != n) {
        result.optimal_weights_ = Vec<f64>();
        return result;
    }

    // Compute inverse of the covariance matrix
    linalg::Matrix<> inv_cov = covariance.inverse();
    if (inv_cov.rows_ == 0) {
        // Non-invertible covariance
        result.optimal_weights_ = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            result.optimal_weights_[i] = 1.0 / static_cast<f64>(n);
        }
        return result;
    }

    // Build vector of ones
    Vec<f64> ones = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) ones[i] = 1.0;

    // Compute scalars
    // a = 1^T * inv_cov * 1
    Vec<f64> inv_ones = inv_cov * ones.slice();
    f64 a = 0.0;
    for (u64 i = 0; i < n; i++) a += ones[i] * inv_ones[i];

    // b = 1^T * inv_cov * mu
    Vec<f64> inv_mu = inv_cov * expected_returns;
    f64 b = 0.0;
    for (u64 i = 0; i < n; i++) b += ones[i] * inv_mu[i];

    // c = mu^T * inv_cov * mu
    f64 c = 0.0;
    for (u64 i = 0; i < n; i++) c += expected_returns[i] * inv_mu[i];

    f64 delta = a * c - b * b;

    if (a < 1e-15 || delta < 1e-15) {
        // Degenerate: return equal-weight portfolio
        result.optimal_weights_ = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            result.optimal_weights_[i] = 1.0 / static_cast<f64>(n);
        }
        return result;
    }

    // Weights = (1/delta) * inv_cov * [ (c - target*b)*1 + (target*a - b)*mu ]
    // where target = max(target_return, b/a + risk_free) if provided,
    // otherwise use the max Sharpe (tangency) portfolio.
    f64 target;
    if (target_return.ok()) {
        target = *target_return;
    } else {
        // Max Sharpe portfolio: w* = inv_cov * (mu - rf*1) / (b - rf*a)
        // normalized to sum to 1.
        target = b / a;  // GMV return, used as baseline
    }

    // Unconstrained efficient portfolio:
    // w* = (c - target*b)/delta * inv_cov_1  +  (target*a - b)/delta * inv_cov_mu
    f64 lambda_1 = (c - target * b) / delta;
    f64 lambda_2 = (target * a - b) / delta;

    result.optimal_weights_ = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) {
        result.optimal_weights_[i] = lambda_1 * inv_ones[i] + lambda_2 * inv_mu[i];
    }

    // If no target_return specified, compute tangency (max Sharpe) portfolio
    if (!target_return.ok()) {
        // Tangency: w* proportional to inv_cov * (mu - rf*1)
        Vec<f64> excess = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            excess[i] = expected_returns[i] - risk_free_rate;
        }
        Vec<f64> w_raw = inv_cov * excess.slice();
        f64 sum_w = 0.0;
        for (u64 i = 0; i < n; i++) sum_w += w_raw[i];

        if (Math::abs(sum_w) > 1e-15) {
            for (u64 i = 0; i < n; i++) {
                result.optimal_weights_[i] = w_raw[i] / sum_w;
            }
        } else {
            // Fallback: equal weight
            for (u64 i = 0; i < n; i++) {
                result.optimal_weights_[i] = 1.0 / static_cast<f64>(n);
            }
        }
    }

    // Compute portfolio expected return
    result.expected_return_ = 0.0;
    for (u64 i = 0; i < n; i++) {
        result.expected_return_ += result.optimal_weights_[i] * expected_returns[i];
    }

    // Compute portfolio variance = w^T * cov * w
    f64 port_var = 0.0;
    for (u64 i = 0; i < n; i++) {
        for (u64 j = 0; j < n; j++) {
            port_var += result.optimal_weights_[i] * covariance(i, j) * result.optimal_weights_[j];
        }
    }
    result.expected_volatility_ = Math::sqrt(port_var);
    result.sharpe_ratio_ = (result.expected_volatility_ > 1e-15)
        ? (result.expected_return_ - risk_free_rate) / result.expected_volatility_
        : 0.0;

    result.optimal_ = true;
    return result;
}

// =========================================================================
// minimum_variance — global minimum variance portfolio
//
// w* = inv_cov * 1 / (1^T * inv_cov * 1)
// =========================================================================
inline OptimizationResult minimum_variance(const linalg::Matrix<>& covariance) noexcept {
    u64 n = covariance.rows_;
    OptimizationResult result;
    result.optimal_ = false;

    if (n == 0 || covariance.cols_ != n) {
        result.optimal_weights_ = Vec<f64>();
        return result;
    }

    linalg::Matrix<> inv_cov = covariance.inverse();
    if (inv_cov.rows_ == 0) {
        result.optimal_weights_ = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            result.optimal_weights_[i] = 1.0 / static_cast<f64>(n);
        }
        return result;
    }

    Vec<f64> ones = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) ones[i] = 1.0;

    Vec<f64> w = inv_cov * ones.slice();
    f64 sum_w = 0.0;
    for (u64 i = 0; i < n; i++) sum_w += w[i];

    result.optimal_weights_ = Vec<f64>::make(n);
    if (Math::abs(sum_w) > 1e-15) {
        for (u64 i = 0; i < n; i++) {
            result.optimal_weights_[i] = w[i] / sum_w;
        }
    } else {
        for (u64 i = 0; i < n; i++) {
            result.optimal_weights_[i] = 1.0 / static_cast<f64>(n);
        }
    }

    // Compute variance
    f64 port_var = 0.0;
    for (u64 i = 0; i < n; i++) {
        for (u64 j = 0; j < n; j++) {
            port_var += result.optimal_weights_[i] * covariance(i, j) * result.optimal_weights_[j];
        }
    }
    result.expected_volatility_ = Math::sqrt(port_var);
    result.expected_return_ = 0.0;  // No expected return input
    result.sharpe_ratio_    = 0.0;
    result.optimal_         = true;

    return result;
}

// =========================================================================
// risk_parity — Equal Risk Contribution (ERC) / Risk Parity portfolio
//
// Minimizes:  sum_i,j  (w_i*(Cov*w)_i - w_j*(Cov*w)_j)^2
// Subject to: sum(w) = 1, w_i >= 0
//
// Uses simple iterative algorithm: w_i^{k+1} = w_i^k / sqrt( (Cov * w^k)_i )
// normalized to sum to 1.  This converges to ERC for long-only portfolios.
// =========================================================================
inline OptimizationResult risk_parity(const linalg::Matrix<>& covariance,
                                       u64 max_iter = 100, f64 tol = 1e-6) noexcept {
    u64 n = covariance.rows_;
    OptimizationResult result;
    result.optimal_ = false;

    if (n == 0 || covariance.cols_ != n) {
        result.optimal_weights_ = Vec<f64>();
        return result;
    }

    // Initial weights: equal weight
    Vec<f64> w = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) w[i] = 1.0 / static_cast<f64>(n);

    for (u64 iter = 0; iter < max_iter; iter++) {
        // Compute portfolio variance contribution: (Cov * w)_i
        Vec<f64> marginal_risk = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 mr = 0.0;
            for (u64 j = 0; j < n; j++) {
                mr += covariance(i, j) * w[j];
            }
            marginal_risk[i] = mr;
        }

        // Risk contribution: w_i * (Cov * w)_i
        Vec<f64> rc = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            rc[i] = w[i] * marginal_risk[i];
            if (rc[i] < 0.0) rc[i] = 0.0;
        }

        // Update: w_i' = w_i / sqrt(marginal_risk_i), renormalized
        Vec<f64> w_new = Vec<f64>::make(n);
        f64 sum_new = 0.0;
        for (u64 i = 0; i < n; i++) {
            if (marginal_risk[i] > 1e-15) {
                w_new[i] = w[i] / Math::sqrt(marginal_risk[i]);
            } else {
                w_new[i] = w[i];
            }
            sum_new += w_new[i];
        }

        // Normalize to sum to 1
        f64 max_diff = 0.0;
        for (u64 i = 0; i < n; i++) {
            w_new[i] /= sum_new;
            f64 diff = Math::abs(w_new[i] - w[i]);
            if (diff > max_diff) max_diff = diff;
            w[i] = w_new[i];
        }

        if (max_diff < tol) break;
    }

    result.optimal_weights_ = spp::move(w);

    // Compute portfolio risk
    f64 port_var = 0.0;
    for (u64 i = 0; i < n; i++) {
        for (u64 j = 0; j < n; j++) {
            port_var += result.optimal_weights_[i] * covariance(i, j) * result.optimal_weights_[j];
        }
    }
    result.expected_volatility_ = Math::sqrt(port_var);
    result.expected_return_     = 0.0;
    result.sharpe_ratio_        = 0.0;
    result.optimal_             = true;

    return result;
}

// =========================================================================
// black_litterman — Black-Litterman model combining market equilibrium
//                   prior with investor views.
//
// Posterior expected returns:
//   E[R] = [ (tau*Sigma)^-1 + P^T * Omega^-1 * P ]^-1 *
//          [ (tau*Sigma)^-1 * Pi + P^T * Omega^-1 * Q ]
//
// Where:
//   Pi     = risk_aversion * Sigma * w_mkt  (implied equilibrium returns)
//   Sigma  = covariance matrix
//   P      = K x N "pick" matrix (maps views to assets)
//   Q      = K x 1 view expected returns
//   Omega  = diag(omega) = K x K uncertainty of views
//   tau    = weight on prior (typically small, e.g. 0.025)
//
// After computing posterior expected returns, feeds into markowitz().
// =========================================================================
inline OptimizationResult black_litterman(
    Slice<const f64> market_caps,
    f64 risk_aversion,
    const linalg::Matrix<>& covariance,
    const linalg::Matrix<>& P,
    Slice<const f64> Q,
    Slice<const f64> omega,
    f64 tau = 0.025) noexcept {

    u64 n = market_caps.length();
    u64 k = Q.length();

    // --- Step 1: Implied equilibrium returns Pi = lambda * Sigma * w_mkt ---
    // Normalize market caps to weights
    f64 sum_cap = 0.0;
    for (u64 i = 0; i < n; i++) sum_cap += market_caps[i];

    Vec<f64> w_mkt = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) {
        w_mkt[i] = (sum_cap > 0.0) ? market_caps[i] / sum_cap : 1.0 / static_cast<f64>(n);
    }

    // Pi = risk_aversion * covariance * w_mkt
    Vec<f64> Pi = covariance * w_mkt.slice();
    for (u64 i = 0; i < n; i++) Pi[i] *= risk_aversion;

    // --- Step 2: Compute posterior expected returns ---
    // E[R] = Pi + tau * Sigma * P^T * (P * tau * Sigma * P^T + Omega)^-1 * (Q - P * Pi)
    //
    // Compute:
    //   M = P * (tau * Sigma) * P^T + Omega    (K x K matrix)
    //   adjustment = tau * Sigma * P^T * M^-1 * (Q - P * Pi)

    // Build tau * Sigma
    linalg::Matrix<> tau_sigma = linalg::Matrix<>::zeros(n, n);
    for (u64 i = 0; i < n; i++) {
        for (u64 j = 0; j < n; j++) {
            tau_sigma(i, j) = tau * covariance(i, j);
        }
    }

    // Build M = P * tau_Sigma * P^T + diag(omega)
    // First: T = P * tau_Sigma  (K x N) * (N x N) = (K x N)
    linalg::Matrix<> T = P * tau_sigma;

    // P^T is N x K
    linalg::Matrix<> P_T = P.transpose();

    // M = T * P^T + diag(omega)   (K x N) * (N x K) = (K x K)
    linalg::Matrix<> M = T * P_T;

    // Add omega to diagonal
    for (u64 i = 0; i < k; i++) {
        M(i, i) += omega[i];
    }

    // Compute Q - P * Pi
    Vec<f64> P_Pi = Vec<f64>::make(k);
    for (u64 i = 0; i < k; i++) {
        f64 sum = 0.0;
        for (u64 j = 0; j < n; j++) {
            sum += P(i, j) * Pi[j];
        }
        P_Pi[i] = sum;
    }

    Vec<f64> Q_minus_P_Pi = Vec<f64>::make(k);
    for (u64 i = 0; i < k; i++) {
        Q_minus_P_Pi[i] = Q[i] - P_Pi[i];
    }

    // Solve M * v = (Q - P*Pi)  for v
    auto v_opt = M.solve(Q_minus_P_Pi.slice());

    Vec<f64> posterior_returns = Vec<f64>::make(n);

    if (v_opt.ok()) {
        Vec<f64> v = spp::move(*v_opt);

        // Compute P^T * v:  an N-vector
        // P is K x N, so P^T is N x K
        // (P^T * v)[j] = sum_{k=0}^{K-1} P^T(j,k) * v[k] = sum_{k} P(k,j) * v[k]
        Vec<f64> P_T_v = Vec<f64>::make(n);
        for (u64 j = 0; j < n; j++) {
            f64 sum = 0.0;
            for (u64 r = 0; r < k; r++) {
                sum += P(r, j) * v[r];
            }
            P_T_v[j] = sum;
        }

        // adjustment = tau_Sigma * (P^T * v):  N-vector
        Vec<f64> adj = tau_sigma * P_T_v.slice();

        // posterior = Pi + adj
        for (u64 i = 0; i < n; i++) {
            posterior_returns[i] = Pi[i] + adj[i];
        }
    } else {
        // Cholesky solve failed — fall back to prior
        for (u64 i = 0; i < n; i++) {
            posterior_returns[i] = Pi[i];
        }
    }

    // --- Step 3: Feed posterior returns into Markowitz ---
    return markowitz(posterior_returns.slice(), covariance, 0.0, {});
}

// =========================================================================
// FrontierPoint — one point on the efficient frontier
// =========================================================================
struct FrontierPoint {
    f64 ret    = 0.0;
    f64 vol    = 0.0;
    f64 sharpe = 0.0;

    SPP_RECORD(FrontierPoint, SPP_FIELD(ret), SPP_FIELD(vol), SPP_FIELD(sharpe));
};

// =========================================================================
// efficient_frontier — sample points along the efficient frontier
//
// Samples `points + 1` return targets between the GMV return and
// the maximum individual asset return, runs markowitz for each,
// and returns the (ret, vol, sharpe) tuples.
// =========================================================================
inline Vec<FrontierPoint> efficient_frontier(
    Slice<const f64> expected_returns,
    const linalg::Matrix<>& covariance,
    f64 risk_free = 0.0,
    u64 points = 50) noexcept {

    u64 n = expected_returns.length();
    Vec<FrontierPoint> frontier = Vec<FrontierPoint>::make(points + 1);
    if (n == 0 || covariance.rows_ != n || covariance.cols_ != n) {
        for (u64 i = 0; i <= points; i++) {
            frontier[i] = {};
        }
        return frontier;
    }

    // Find return range
    f64 min_ret = expected_returns[0];
    f64 max_ret = expected_returns[0];
    for (u64 i = 1; i < n; i++) {
        if (expected_returns[i] < min_ret) min_ret = expected_returns[i];
        if (expected_returns[i] > max_ret) max_ret = expected_returns[i];
    }

    // GMV return as lower bound
    OptimizationResult gmv = markowitz(expected_returns, covariance, risk_free, Opt<f64>{min_ret});
    f64 gmv_ret = gmv.expected_return_;

    f64 step = (max_ret - gmv_ret) / static_cast<f64>(points);
    if (step < 1e-10) step = 0.0001;

    for (u64 i = 0; i <= points; i++) {
        f64 target = gmv_ret + step * static_cast<f64>(i);
        OptimizationResult opt = markowitz(expected_returns, covariance, risk_free, Opt<f64>{target});
        frontier[i].ret    = opt.expected_return_;
        frontier[i].vol    = opt.expected_volatility_;
        frontier[i].sharpe = opt.sharpe_ratio_;
    }

    return frontier;
}

}  // namespace spp::quant
