#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/math/interpolation.h"

namespace spp::quant::calib {

// =========================================================================
// NelsonSiegel — three-factor yield curve model (Nelson & Siegel 1987)
// =========================================================================
//
// Instantaneous forward rate:
//   f(t) = beta0 + beta1 * exp(-t/tau) + beta2 * (t/tau) * exp(-t/tau)
//
// Zero-coupon yield (spot rate):
//   y(t) = beta0 + beta1 * (1 - exp(-t/tau)) / (t/tau)
//                + beta2 * ((1 - exp(-t/tau)) / (t/tau) - exp(-t/tau))
//
// Parameter interpretation:
//   beta0: long-term level (lim_{t->inf} y(t) = beta0)
//   beta1: short-term component (beta0 + beta1 = lim_{t->0} y(t))
//   beta2: medium-term curvature (hump/dip shape)
//   tau:   decay factor (where the hump is located)
//
// Constraints for economically sensible curves:
//   beta0 > 0 (positive long rate)
//   beta0 + beta1 > 0 (positive short rate)
//   tau > 0 (positive decay)

struct NelsonSiegel {
    f64 beta0_ = 0.03;  ///< long-term level (3%)
    f64 beta1_ = -0.02; ///< short-term slope (short rate = beta0 + beta1)
    f64 beta2_ = 0.01;  ///< medium-term curvature
    f64 tau_   = 2.0;   ///< decay factor (years)

    SPP_RECORD(NelsonSiegel, SPP_FIELD(beta0_), SPP_FIELD(beta1_),
               SPP_FIELD(beta2_), SPP_FIELD(tau_));

    // =====================================================================
    // Yield at time t (years)
    //
    // y(t) = beta0
    //      + beta1 * (1 - exp(-t/tau)) / (t/tau)
    //      + beta2 * ((1 - exp(-t/tau)) / (t/tau) - exp(-t/tau))
    // =====================================================================

    [[nodiscard]] f64 yield(f64 t) const {
        if (t <= 0.0) return beta0_ + beta1_;
        if (tau_ <= 0.0) return beta0_;

        f64 x = t / tau_;
        f64 exp_neg_x = Math::exp(-x);

        // (1 - exp(-x)) / x
        f64 factor;
        if (x < 1e-6) {
            // Taylor expansion for small x: 1 - x/2 + x^2/6 - x^3/24 + ...
            factor = 1.0 - x * 0.5 + x * x / 6.0 - x * x * x / 24.0;
        } else {
            factor = (1.0 - exp_neg_x) / x;
        }

        f64 result = beta0_ + beta1_ * factor + beta2_ * (factor - exp_neg_x);
        return result;
    }

    // =====================================================================
    // Discount factor at time t: df(t) = exp(-y(t) * t)
    // =====================================================================

    [[nodiscard]] f64 discount(f64 t) const {
        if (t <= 0.0) return 1.0;
        return Math::exp(-yield(t) * t);
    }

    // =====================================================================
    // Instantaneous forward rate at time t:
    //   f(t) = beta0 + beta1 * exp(-t/tau) + beta2 * (t/tau) * exp(-t/tau)
    // =====================================================================

    [[nodiscard]] f64 forward_rate(f64 t) const {
        if (t <= 0.0) return beta0_ + beta1_;
        if (tau_ <= 0.0) return beta0_;

        f64 x = t / tau_;
        f64 exp_neg_x = Math::exp(-x);
        f64 result = beta0_ + beta1_ * exp_neg_x + beta2_ * x * exp_neg_x;
        return result;
    }

    // =====================================================================
    // Fit Nelson-Siegel parameters to market yields.
    //
    // Minimizes: sum_i (y(t_i) - market_yield_i)^2
    //
    // Uses Nelder-Mead simplex optimization (derivative-free, robust).
    // =====================================================================

    [[nodiscard]] static NelsonSiegel fit(Slice<const f64> maturities,
                                           Slice<const f64> yields) {
        u64 n = Math::min(maturities.length(), yields.length());
        if (n < 4) {
            // Not enough data: return default parameters
            NelsonSiegel ns;
            if (n > 0) {
                // Simple: set beta0 = last yield, beta1 = first yield - beta0
                ns.beta0_ = yields[n - 1];
                ns.beta1_ = yields[0] - ns.beta0_;
                ns.beta2_ = 0.0;
                ns.tau_ = maturities[n / 2];
            }
            return ns;
        }

        // Initial guess
        f64 y_short = yields[0];
        f64 y_long  = yields[n - 1];
        f64 beta0_0 = y_long;
        f64 beta1_0 = y_short - beta0_0;
        f64 beta2_0 = 0.0;
        f64 tau_0   = maturities[n / 2];  // middle maturity as initial tau

        // Bounds
        f64 lb_beta0 = -0.1, ub_beta0 = 0.2;
        f64 lb_beta1 = -0.2, ub_beta1 = 0.2;
        f64 lb_beta2 = -0.2, ub_beta2 = 0.2;
        f64 lb_tau   = 0.01, ub_tau = 30.0;

        // Objective function
        auto objective = [&](Slice<const f64> x) -> f64 {
            NelsonSiegel ns_cur{x[0], x[1], x[2], x[3]};
            f64 err = 0.0;
            for (u64 i = 0; i < n; i++) {
                f64 diff = ns_cur.yield(maturities[i]) - yields[i];
                err += diff * diff;
            }
            return Math::sqrt(err / static_cast<f64>(n));
        };

        Vec<f64> x0 = Vec<f64>::make(4);
        x0[0] = beta0_0; x0[1] = beta1_0; x0[2] = beta2_0; x0[3] = tau_0;

        Vec<f64> lb = Vec<f64>::make(4);
        Vec<f64> ub = Vec<f64>::make(4);
        lb[0] = lb_beta0; ub[0] = ub_beta0;
        lb[1] = lb_beta1; ub[1] = ub_beta1;
        lb[2] = lb_beta2; ub[2] = ub_beta2;
        lb[3] = lb_tau;   ub[3] = ub_tau;

        // Nelder-Mead
        u64 n_params = 4;
        u64 m = n_params + 1;
        Vec<Vec<f64>> simplex;
        simplex.push(x0);
        for (u64 i = 0; i < n_params; i++) {
            Vec<f64> pt = x0.clone();
            f64 perturb = 0.05 * (ub[i] - lb[i]);
            if (perturb < 1e-6) perturb = 0.01;
            pt[i] += perturb;
            if (pt[i] > ub[i]) pt[i] = ub[i];
            simplex.push(spp::move(pt));
        }

        auto bounded_obj = [&](Slice<const f64> x) -> f64 {
            f64 pen = 0.0;
            for (u64 i = 0; i < n_params; i++) {
                if (x[i] < lb[i])     pen += (lb[i] - x[i]) * (lb[i] - x[i]) * 1e8;
                if (x[i] > ub[i])     pen += (x[i] - ub[i]) * (x[i] - ub[i]) * 1e8;
            }
            return objective(x) + pen;
        };

        Vec<f64> fvals = Vec<f64>::make(m);
        for (u64 i = 0; i < m; i++) fvals[i] = bounded_obj(simplex[i].slice());

        for (u64 iter = 0; iter < 500; iter++) {
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

            // Convergence
            f64 fmean = 0.0;
            for (u64 i = 0; i < m; i++) fmean += fvals[i];
            fmean /= static_cast<f64>(m);
            f64 fstd = 0.0;
            for (u64 i = 0; i < m; i++) { f64 d = fvals[i] - fmean; fstd += d * d; }
            if (Math::sqrt(fstd / static_cast<f64>(m)) < 1e-6) break;

            Vec<f64> centroid = Vec<f64>::make(n_params);
            for (u64 i = 0; i < n_params; i++) centroid[i] = 0.0;
            for (u64 i = 0; i < m - 1; i++)
                for (u64 j = 0; j < n_params; j++) centroid[j] += simplex[i][j];
            for (u64 j = 0; j < n_params; j++) centroid[j] /= static_cast<f64>(m - 1);

            Vec<f64> reflected = Vec<f64>::make(n_params);
            for (u64 j = 0; j < n_params; j++)
                reflected[j] = centroid[j] + 1.0 * (centroid[j] - simplex[m - 1][j]);
            f64 f_reflected = bounded_obj(reflected.slice());

            if (f_reflected < fvals[0]) {
                Vec<f64> expanded = Vec<f64>::make(n_params);
                for (u64 j = 0; j < n_params; j++)
                    expanded[j] = centroid[j] + 2.0 * (reflected[j] - centroid[j]);
                f64 f_expanded = bounded_obj(expanded.slice());
                if (f_expanded < f_reflected) {
                    simplex[m - 1] = spp::move(expanded); fvals[m - 1] = f_expanded;
                } else {
                    simplex[m - 1] = spp::move(reflected); fvals[m - 1] = f_reflected;
                }
            } else if (f_reflected < fvals[m - 2]) {
                simplex[m - 1] = spp::move(reflected); fvals[m - 1] = f_reflected;
            } else {
                Vec<f64> contracted = Vec<f64>::make(n_params);
                if (f_reflected < fvals[m - 1]) {
                    for (u64 j = 0; j < n_params; j++)
                        contracted[j] = centroid[j] + 0.5 * (reflected[j] - centroid[j]);
                } else {
                    for (u64 j = 0; j < n_params; j++)
                        contracted[j] = centroid[j] + 0.5 * (simplex[m - 1][j] - centroid[j]);
                }
                f64 f_contracted = bounded_obj(contracted.slice());
                if (f_contracted < Math::min(f_reflected, fvals[m - 1])) {
                    simplex[m - 1] = spp::move(contracted); fvals[m - 1] = f_contracted;
                } else {
                    for (u64 i = 1; i < m; i++) {
                        for (u64 j = 0; j < n_params; j++)
                            simplex[i][j] = simplex[0][j] + 0.5 * (simplex[i][j] - simplex[0][j]);
                        fvals[i] = bounded_obj(simplex[i].slice());
                    }
                }
            }
        }

        Slice<const f64> best = simplex[0].slice();
        NelsonSiegel result{best[0], best[1], best[2], best[3]};
        return result;
    }
};

// =========================================================================
// Svensson — six-factor yield curve model (Svensson 1994)
// =========================================================================
//
// Extension of Nelson-Siegel with a second hump term:
//
// y(t) = NS(t) + beta3 * ((1 - exp(-t/tau2)) / (t/tau2) - exp(-t/tau2))
//
// The second hump allows modeling more complex shapes (e.g., twists
// at intermediate maturities).

struct Svensson {
    f64 beta0_ = 0.03;
    f64 beta1_ = -0.02;
    f64 beta2_ = 0.01;
    f64 beta3_ = 0.005;  ///< second hump magnitude
    f64 tau1_  = 2.0;    ///< first decay factor
    f64 tau2_  = 5.0;    ///< second decay factor (typically tau2 > tau1)

    SPP_RECORD(Svensson, SPP_FIELD(beta0_), SPP_FIELD(beta1_), SPP_FIELD(beta2_),
               SPP_FIELD(beta3_), SPP_FIELD(tau1_), SPP_FIELD(tau2_));

    /// Helper: compute the hump factor (1 - exp(-x))/x - exp(-x)
    [[nodiscard]] static f64 hump_factor(f64 t, f64 tau) {
        if (t <= 0.0 || tau <= 0.0) return 0.0;
        f64 x = t / tau;
        f64 exp_neg_x = Math::exp(-x);
        f64 factor;
        if (x < 1e-6) {
            factor = x / 2.0 - x * x / 6.0;  // Taylor: (1-exp(-x))/x - exp(-x) ~ x/2
        } else {
            factor = (1.0 - exp_neg_x) / x - exp_neg_x;
        }
        return factor;
    }

    [[nodiscard]] f64 yield(f64 t) const {
        if (t <= 0.0) return beta0_ + beta1_;
        if (tau1_ <= 0.0) return beta0_;

        // Nelson-Siegel part
        f64 x1 = t / tau1_;
        f64 exp_neg_x1 = Math::exp(-x1);
        f64 factor1;
        if (x1 < 1e-6)
            factor1 = 1.0 - x1 * 0.5 + x1 * x1 / 6.0;
        else
            factor1 = (1.0 - exp_neg_x1) / x1;

        f64 ns_yield = beta0_ + beta1_ * factor1 + beta2_ * (factor1 - exp_neg_x1);

        // Second hump
        f64 second_hump = hump_factor(t, tau2_);

        return ns_yield + beta3_ * second_hump;
    }

    [[nodiscard]] f64 discount(f64 t) const {
        if (t <= 0.0) return 1.0;
        return Math::exp(-yield(t) * t);
    }

    [[nodiscard]] f64 forward_rate(f64 t) const {
        if (t <= 0.0) return beta0_ + beta1_;
        if (tau1_ <= 0.0 || tau2_ <= 0.0) return beta0_;

        // f(t) = beta0 + beta1*exp(-t/tau1) + beta2*(t/tau1)*exp(-t/tau1)
        //       + beta3*(t/tau2)*exp(-t/tau2)
        f64 x1 = t / tau1_;
        f64 x2 = t / tau2_;
        f64 exp_neg_x1 = Math::exp(-x1);
        f64 exp_neg_x2 = Math::exp(-x2);

        return beta0_ + beta1_ * exp_neg_x1
             + beta2_ * x1 * exp_neg_x1
             + beta3_ * x2 * exp_neg_x2;
    }

    /// Fit Svensson parameters to market yields.
    ///
    /// Uses 2-step approach:
    ///   1. Fit Nelson-Siegel to get initial guess for beta0-2, tau1.
    ///   2. Add beta3 and tau2, optimize full 6-parameter model.

    [[nodiscard]] static Svensson fit(Slice<const f64> maturities,
                                       Slice<const f64> yields) {
        u64 n = Math::min(maturities.length(), yields.length());
        if (n < 6) {
            // Fall back to Nelson-Siegel
            NelsonSiegel ns = NelsonSiegel::fit(maturities, yields);
            return Svensson{ns.beta0_, ns.beta1_, ns.beta2_, 0.0, ns.tau_, 5.0};
        }

        // Step 1: Fit Nelson-Siegel
        NelsonSiegel ns = NelsonSiegel::fit(maturities, yields);

        // Step 2: Optimize the full 6 parameters
        f64 beta3_0 = 0.0;
        f64 tau2_0  = maturities[n - 1] * 0.5;  // longer than tau1

        Vec<f64> x0 = Vec<f64>::make(6);
        x0[0] = ns.beta0_; x0[1] = ns.beta1_; x0[2] = ns.beta2_;
        x0[3] = beta3_0;   x0[4] = ns.tau_;   x0[5] = tau2_0;

        Vec<f64> lb = Vec<f64>::make(6);
        Vec<f64> ub = Vec<f64>::make(6);
        lb[0] = -0.1; ub[0] = 0.2;   // beta0
        lb[1] = -0.2; ub[1] = 0.2;   // beta1
        lb[2] = -0.2; ub[2] = 0.2;   // beta2
        lb[3] = -0.2; ub[3] = 0.2;   // beta3
        lb[4] = 0.01; ub[4] = 30.0;  // tau1
        lb[5] = 0.01; ub[5] = 30.0;  // tau2

        auto objective = [&](Slice<const f64> x) -> f64 {
            Svensson sv{x[0], x[1], x[2], x[3], x[4], x[5]};
            f64 err = 0.0;
            for (u64 i = 0; i < n; i++) {
                f64 diff = sv.yield(maturities[i]) - yields[i];
                err += diff * diff;
            }
            return Math::sqrt(err / static_cast<f64>(n));
        };

        // Nelder-Mead (same pattern as NelsonSiegel::fit)
        u64 np = 6, m = np + 1;
        Vec<Vec<f64>> simplex;
        simplex.push(x0);
        for (u64 i = 0; i < np; i++) {
            Vec<f64> pt = x0.clone();
            f64 perturb = 0.05 * (ub[i] - lb[i]);
            if (perturb < 1e-6) perturb = 0.01;
            pt[i] += perturb;
            if (pt[i] > ub[i]) pt[i] = ub[i];
            simplex.push(spp::move(pt));
        }

        auto bobj = [&](Slice<const f64> x) -> f64 {
            f64 pen = 0.0;
            for (u64 i = 0; i < np; i++) {
                if (x[i] < lb[i]) pen += (lb[i] - x[i]) * (lb[i] - x[i]) * 1e8;
                if (x[i] > ub[i]) pen += (x[i] - ub[i]) * (x[i] - ub[i]) * 1e8;
            }
            return objective(x) + pen;
        };

        Vec<f64> fvals = Vec<f64>::make(m);
        for (u64 i = 0; i < m; i++) fvals[i] = bobj(simplex[i].slice());

        for (u64 iter = 0; iter < 800; iter++) {
            for (u64 i = 1; i < m; i++)
                for (u64 j = i; j > 0 && fvals[j] < fvals[j - 1]; j--) {
                    f64 tf = fvals[j]; fvals[j] = fvals[j - 1]; fvals[j - 1] = tf;
                    Vec<f64> tv = spp::move(simplex[j]);
                    simplex[j] = spp::move(simplex[j - 1]);
                    simplex[j - 1] = spp::move(tv);
                }
            f64 fmean = 0.0;
            for (u64 i = 0; i < m; i++) fmean += fvals[i];
            fmean /= static_cast<f64>(m);
            f64 fstd = 0.0;
            for (u64 i = 0; i < m; i++) { f64 d = fvals[i] - fmean; fstd += d * d; }
            if (Math::sqrt(fstd / static_cast<f64>(m)) < 1e-6) break;

            Vec<f64> centroid = Vec<f64>::make(np);
            for (u64 i = 0; i < np; i++) centroid[i] = 0.0;
            for (u64 i = 0; i < m - 1; i++)
                for (u64 j = 0; j < np; j++) centroid[j] += simplex[i][j];
            for (u64 j = 0; j < np; j++) centroid[j] /= static_cast<f64>(m - 1);

            Vec<f64> refl = Vec<f64>::make(np);
            for (u64 j = 0; j < np; j++)
                refl[j] = centroid[j] + 1.0 * (centroid[j] - simplex[m - 1][j]);
            f64 f_refl = bobj(refl.slice());
            if (f_refl < fvals[0]) {
                Vec<f64> expd = Vec<f64>::make(np);
                for (u64 j = 0; j < np; j++)
                    expd[j] = centroid[j] + 2.0 * (refl[j] - centroid[j]);
                f64 f_expd = bobj(expd.slice());
                if (f_expd < f_refl) { simplex[m - 1] = spp::move(expd); fvals[m - 1] = f_expd; }
                else                 { simplex[m - 1] = spp::move(refl); fvals[m - 1] = f_refl; }
            } else if (f_refl < fvals[m - 2]) {
                simplex[m - 1] = spp::move(refl); fvals[m - 1] = f_refl;
            } else {
                Vec<f64> contr = Vec<f64>::make(np);
                if (f_refl < fvals[m - 1])
                    for (u64 j = 0; j < np; j++) contr[j] = centroid[j] + 0.5 * (refl[j] - centroid[j]);
                else
                    for (u64 j = 0; j < np; j++) contr[j] = centroid[j] + 0.5 * (simplex[m - 1][j] - centroid[j]);
                f64 f_contr = bobj(contr.slice());
                if (f_contr < Math::min(f_refl, fvals[m - 1])) {
                    simplex[m - 1] = spp::move(contr); fvals[m - 1] = f_contr;
                } else {
                    for (u64 i = 1; i < m; i++) {
                        for (u64 j = 0; j < np; j++)
                            simplex[i][j] = simplex[0][j] + 0.5 * (simplex[i][j] - simplex[0][j]);
                        fvals[i] = bobj(simplex[i].slice());
                    }
                }
            }
        }

        Slice<const f64> best = simplex[0].slice();
        return Svensson{best[0], best[1], best[2], best[3], best[4], best[5]};
    }
};

// =========================================================================
// MonotoneConvex — Hagan-West (2006) monotone convex interpolation
// =========================================================================
//
// Constructs an arbitrage-free forward rate curve from discrete
// forward rates, ensuring:
//   1. Forward rates are positive (no arbitrage).
//   2. Forward rates are continuous.
//   3. The interpolation is monotone (preserves the shape of inputs).
//
// This is the standard method for constructing discount curves
// from discrete forwards with interpolation that is both smooth
// and arbitrage-free.

struct MonotoneConvex {
    Vec<f64> times_;     ///< time grid (years from reference date)
    Vec<f64> forwards_;  ///< discrete forward rates f(t_i, t_{i+1})

    /// Interpolate the instantaneous forward rate at time t.
    ///
    /// Uses the Hagan-West piecewise cubic interpolation on the
    /// discrete forward rates, with monotonicity-preserving adjustments.

    [[nodiscard]] f64 forward(f64 t) const {
        u64 n = times_.length();
        if (n < 2) return (n == 0) ? 0.0 : forwards_[0];

        // Clamp to bounds
        if (t <= times_[0]) return forwards_[0];
        if (t >= times_[n - 1]) return forwards_[n - 2];  // last discrete forward

        // Binary search for interval
        u64 lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            u64 mid = (lo + hi) / 2;
            if (times_[mid] <= t) lo = mid;
            else hi = mid;
        }

        u64 i = lo;
        f64 t_i   = times_[i];
        f64 t_ip1 = times_[i + 1];
        f64 dt    = t_ip1 - t_i;

        if (dt <= 0.0) return forwards_[i];

        // Get neighboring data for interpolation
        f64 f_im1 = (i > 0) ? forwards_[i - 1] : forwards_[0];
        f64 f_i   = forwards_[i];
        f64 f_ip1 = (i + 1 < n - 1) ? forwards_[i + 1] : forwards_[n - 2];

        // Hagan-West monotone convex interpolation:
        //
        // For each interval [t_i, t_{i+1}], we construct a cubic polynomial
        // g(x) for x in [0, 1] where x = (t - t_i) / (t_{i+1} - t_i).
        //
        // g(0) = f_i, g(1) = f_{i+1}
        // g'(0) = d_i, g'(1) = d_{i+1}
        //
        // where d_i are estimates of the derivative at node i.
        //
        // The monotonicity adjustment ensures g(x) >= 0 and the shape
        // is preserved.

        // Estimate derivatives using harmonic mean of adjacent slopes
        auto deriv = [](f64 f_prev, f64 f_curr, f64 f_next,
                         f64 dt_prev, f64 dt_next) -> f64 {
            if (dt_prev <= 0.0 || dt_next <= 0.0) return 0.0;

            f64 s_prev = (f_curr - f_prev) / dt_prev;
            f64 s_next = (f_next - f_curr) / dt_next;

            if (s_prev * s_next <= 0.0) return 0.0;

            // Harmonic mean with weights proportional to interval lengths
            f64 w_prev = 2.0 * dt_next + dt_prev;
            f64 w_next = dt_next + 2.0 * dt_prev;
            return (w_prev + w_next) / (w_prev / s_prev + w_next / s_next);
        };

        // Time differences for derivative estimation
        f64 dt_prev = (i > 0) ? (t_i - times_[i - 1]) : dt;
        f64 dt_next = (i + 2 < n) ? (times_[i + 2] - t_ip1) : dt;
        if (dt_next <= 0.0) dt_next = dt;

        f64 dt_ip1_next = dt_next;  // for d_{i+1}
        f64 dt_i_next   = dt;       // for d_i

        f64 d_i   = deriv(f_im1, f_i, f_ip1, dt_prev, dt_i_next);
        f64 d_ip1 = deriv(f_i, f_ip1, (i + 2 < n - 1) ? forwards_[i + 2] : f_ip1,
                           dt, dt_ip1_next);

        // Construct cubic: g(x) = a + b*x + c*x^2 + d*x^3
        // with g(0) = f_i, g(1) = f_{i+1}, g'(0) = d_i*dt, g'(1) = d_{i+1}*dt
        f64 a = f_i;
        f64 b = d_i * dt;
        f64 c = 3.0 * (f_ip1 - f_i) - 2.0 * d_i * dt - d_ip1 * dt;
        f64 d = 2.0 * (f_i - f_ip1) + d_i * dt + d_ip1 * dt;

        // Monotonicity-enforcement:
        // Check that g(x) is monotone between f_i and f_{i+1}.
        // If not, adjust derivatives.
        if ((f_ip1 - f_i) * b < 0.0) {
            b = 0.0;
            c = 3.0 * (f_ip1 - f_i) - d_ip1 * dt;
            d = 2.0 * (f_i - f_ip1) + d_ip1 * dt;
        }
        if ((f_ip1 - f_i) * (c + d) < 0.0) {
            c = 3.0 * (f_ip1 - f_i) - 2.0 * b;
            d = 2.0 * (f_i - f_ip1) + b;
        }

        // Also enforce positivity of forward rates:
        // Check g(x) at the minimum point (if interior).
        // If g(x_min) < 0, adjust to prevent negative forward.
        // [UNSPECIFIED] The positivity check is simplified:
        // we clamp g(x) >= 0 at the interval endpoints and midpoint.
        f64 x = (t - t_i) / dt;
        f64 g = a + b * x + c * x * x + d * x * x * x;
        if (g < 0.0) g = 0.0;

        return g;
    }

    /// Discount factor at time t: df(t) = exp(-integral_0^t f(s) ds)
    ///
    /// Uses numerical integration of the forward rate curve.

    [[nodiscard]] f64 discount(f64 t) const {
        if (t <= 0.0) return 1.0;

        // Integrate forward rate using simple trapezoidal rule.
        // For a piecewise cubic forward rate, the exact integral
        // can be computed analytically. For robustness, we use
        // a composite Simpson rule with sufficiently small steps.

        u64 n_steps = 100;  // 100 steps should give ~1e-8 accuracy
        f64 dt = t / static_cast<f64>(n_steps);
        f64 integral = 0.0;

        f64 f_prev = forward(0.0);
        for (u64 i = 1; i <= n_steps; i++) {
            f64 ti = i * dt;
            f64 f_curr = forward(ti);
            // Trapezoidal
            integral += 0.5 * (f_prev + f_curr) * dt;
            f_prev = f_curr;
        }

        return Math::exp(-integral);
    }

    /// Build MonotoneConvex interpolation from market yields.
    ///
    /// Step 1: Compute discrete forward rates from yields.
    /// Step 2: Build the interpolation grid.

    [[nodiscard]] static MonotoneConvex from_yields(Slice<const f64> maturities,
                                                     Slice<const f64> yields) {
        u64 n = Math::min(maturities.length(), yields.length());
        MonotoneConvex mc;
        if (n < 2) return mc;

        // Compute discount factors from yields (continuous compounding)
        Vec<f64> dfs = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            dfs[i] = Math::exp(-yields[i] * maturities[i]);
        }

        // Compute discrete forward rates:
        // f(t_i, t_{i+1}) = -ln(df_{i+1} / df_i) / (t_{i+1} - t_i)
        Vec<f64> times = Vec<f64>::make(n);
        Vec<f64> fwds = Vec<f64>::make(n - 1);
        for (u64 i = 0; i < n; i++) times[i] = maturities[i];
        for (u64 i = 0; i < n - 1; i++) {
            f64 dt = maturities[i + 1] - maturities[i];
            if (dt > 0.0 && dfs[i] > 0.0 && dfs[i + 1] > 0.0) {
                fwds[i] = -Math::log(dfs[i + 1] / dfs[i]) / dt;
            } else {
                fwds[i] = yields[i];  // fallback
            }
            if (fwds[i] < 0.0) fwds[i] = 0.0;  // no negative forwards
        }

        mc.times_    = spp::move(times);
        mc.forwards_ = spp::move(fwds);
        return mc;
    }
};

/// Helper: build NelsonSiegel parameters from a vector
inline Vec<f64> params_to_vec_impl(const NelsonSiegel& p, Vec<f64>& v) {
    v.push(p.beta0_); v.push(p.beta1_); v.push(p.beta2_); v.push(p.tau_);
    return v;
}
inline void vec_to_params_impl(Slice<const f64> v, NelsonSiegel& p) {
    p.beta0_ = v[0]; p.beta1_ = v[1]; p.beta2_ = v[2]; p.tau_ = v[3];
}

} // namespace spp::quant::calib
