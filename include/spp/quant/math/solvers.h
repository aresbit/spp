#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <cmath>

namespace spp::quant::solvers {

// ============================================================
// Bisection method: find x such that f(x) = target in [low, high]
// ============================================================
// Requires: f(low) - target and f(high) - target have opposite signs.
// Returns Opt<f64> — None if bracket is invalid or max iterations exceeded.

template<typename F>
[[nodiscard]] inline Opt<f64> bisection(F&& f, f64 target, f64 low, f64 high,
                                         f64 tol = 1e-8, u64 max_iter = 100) noexcept {
    f64 f_low = f(low) - target;
    f64 f_high = f(high) - target;

    // Check if we already have a root at an endpoint
    if (Math::abs(f_low) < tol) return Opt{low};
    if (Math::abs(f_high) < tol) return Opt{high};

    // Check bracket validity
    if (f_low * f_high > 0.0) return {};

    f64 mid = 0.0;
    f64 f_mid = 0.0;

    for (u64 iter = 0; iter < max_iter; iter++) {
        mid = 0.5 * (low + high);
        f_mid = f(mid) - target;

        if (Math::abs(f_mid) < tol) return Opt{mid};

        // Convergence check on interval width
        if (Math::abs(high - low) < tol * Math::abs(mid) + tol) return Opt{mid};

        if (f_low * f_mid <= 0.0) {
            // Root is in [low, mid]
            high = mid;
            f_high = f_mid;
        } else {
            // Root is in [mid, high]
            low = mid;
            f_low = f_mid;
        }
    }

    return Opt{mid};
}

// ============================================================
// Newton-Raphson method: find x such that f(x) = target
// ============================================================
// Requires derivative function df. Starts from guess.
// Returns Opt<f64> — None if fails to converge.

template<typename F, typename DF>
[[nodiscard]] inline Opt<f64> newton_raphson(F&& f, DF&& df, f64 target,
                                              f64 guess, f64 tol = 1e-8,
                                              u64 max_iter = 100) noexcept {
    f64 x = guess;

    for (u64 iter = 0; iter < max_iter; iter++) {
        f64 fx = f(x) - target;
        f64 dfx = df(x);

        if (Math::abs(fx) < tol) return Opt{x};

        // Protect against zero or near-zero derivative
        if (Math::abs(dfx) < 1e-15) return {};

        f64 dx = fx / dfx;
        x = x - dx;

        // Check convergence of the step
        if (Math::abs(dx) < tol * Math::abs(x) + tol) return Opt{x};

        // Safeguard against divergence
        if (Math::abs(x) > 1e15) return {};
    }

    return Opt{x};
}

// ============================================================
// Brent's method: robust root-finding combining bisection,
// secant, and inverse quadratic interpolation.
// ============================================================
// Standard Brent-Dekker algorithm as described in
// Brent, R.P. (1973), "Algorithms for Minimization without Derivatives"

template<typename F>
[[nodiscard]] inline Opt<f64> brent(F&& f, f64 target, f64 low, f64 high,
                                     f64 tol = 1e-8, u64 max_iter = 100) noexcept {
    f64 a = low;
    f64 b = high;
    f64 fa = f(a) - target;
    f64 fb = f(b) - target;

    if (Math::abs(fa) < tol) return Opt{a};
    if (Math::abs(fb) < tol) return Opt{b};

    // Ensure the root is bracketed
    if (fa * fb > 0.0) return {};

    // c is the previous iterate (initially equal to a)
    f64 c = a;
    f64 fc = fa;
    f64 d = b - a;  // step size before last
    f64 e = d;      // step size at last iteration

    for (u64 iter = 0; iter < max_iter; iter++) {
        // Reorder so that |fb| <= |fa|
        if (Math::abs(fc) < Math::abs(fb)) {
            a = b; b = c; c = a;
            fa = fb; fb = fc; fc = fa;
        }

        f64 tol_act = 2.0 * Limits<f64>::epsilon() * Math::abs(b) + 0.5 * tol;
        f64 xm = 0.5 * (c - b);

        // Convergence check
        if (Math::abs(xm) <= tol_act || Math::abs(fb) < tol) return Opt{b};

        if (Math::abs(e) >= tol_act && Math::abs(fa) > Math::abs(fb)) {
            // Try inverse quadratic interpolation
            f64 s = fb / fa;
            f64 p, q;
            if (a == c) {
                // Linear interpolation
                p = 2.0 * xm * s;
                q = 1.0 - s;
            } else {
                // Inverse quadratic interpolation
                q = fa / fc;
                f64 r = fb / fc;
                p = s * (2.0 * xm * q * (q - r) - (b - a) * (r - 1.0));
                q = (q - 1.0) * (r - 1.0) * (s - 1.0);
            }

            // Ensure p is positive
            if (p > 0.0) q = -q;
            else p = -p;

            // Check if interpolation is acceptable
            s = e;
            e = d;
            if (2.0 * p < 3.0 * xm * q - Math::abs(tol_act * q) && p < Math::abs(0.5 * s * q)) {
                // Accept interpolation
                d = p / q;
            } else {
                // Reject, use bisection instead
                d = xm;
                e = d;
            }
        } else {
            // Bound step too small — use bisection
            d = xm;
            e = d;
        }

        // Advance: a becomes the previous b, b becomes the new point
        a = b;
        fa = fb;

        // Evaluate at the new point
        if (Math::abs(d) > tol_act) {
            b = b + d;
        } else {
            b = b + (xm > 0.0 ? tol_act : -tol_act);
        }

        fb = f(b) - target;

        // Update c to maintain the bracket
        if ((fb > 0.0 && fc > 0.0) || (fb < 0.0 && fc < 0.0)) {
            c = a;
            fc = fa;
            d = b - a;
            e = d;
        }
    }

    return Opt{b};
}

// ============================================================
// Convenience: solve f(x) = 0 using Brent's method
// ============================================================

template<typename F>
[[nodiscard]] inline Opt<f64> solve_brent(F&& f, f64 low, f64 high,
                                           f64 tol = 1e-8, u64 max_iter = 100) noexcept {
    return brent(spp::forward<F>(f), 0.0, low, high, tol, max_iter);
}

// ============================================================
// Implied volatility solver
// ============================================================
// Given a pricing function price_fn(vol) and a market price,
// find the volatility that makes price_fn(vol) = target_price.
// Uses brent's method with a reasonable vol bracket [1e-6, 10.0].
// Returns Opt<f64> — None if no solution found.

template<typename F>
[[nodiscard]] inline Opt<f64> implied_volatility(F&& price_fn, f64 target_price,
                                                  f64 low = 1e-6, f64 high = 10.0,
                                                  f64 tol = 1e-8) noexcept {
    // Check bracket validity: price function should be monotonic in vol.
    // We handle this by checking if the root is bracketed, and widening
    // the bracket if needed.
    auto f_target = [&](f64 vol) -> f64 {
        return price_fn(vol);
    };

    Opt<f64> result = brent(f_target, target_price, low, high, tol);

    if (result.ok()) return result;

    // If bracket failed, try expanding the lower bound
    f64 try_low = low * 0.1;
    for (u32 i = 0; i < 10 && !result.ok(); i++) {
        if (try_low < 1e-12) break;
        result = brent(f_target, target_price, try_low, high, tol);
        try_low *= 0.1;
    }

    // Try expanding the upper bound
    f64 try_high = high * 10.0;
    for (u32 i = 0; i < 10 && !result.ok(); i++) {
        if (try_high > 1e3) break;
        result = brent(f_target, target_price, low, try_high, tol);
        try_high *= 10.0;
    }

    return result;
}

} // namespace spp::quant::solvers
