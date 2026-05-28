#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <cmath>

namespace spp::quant::interp {

// ============================================================
// Interpolation method enum
// ============================================================

enum struct Interp_Method : u8 {
    Linear = 0,
    LogLinear = 1,
    CubicSpline = 2,
    MonotonicCubic = 3,
    FlatForward = 4
};

// ============================================================
// Interpolation template class
// ============================================================
// x_ must be sorted ascending. Extrapolation is flat beyond bounds.

template<typename A = Mdefault>
struct Interpolation {
    Vec<f64, A> x_;  // knots (sorted ascending)
    Vec<f64, A> y_;  // values at knots
    Interp_Method method_;
    Vec<f64, A> impl_;  // second derivatives for spline methods

    // Factory methods
    [[nodiscard]] static Interpolation linear(Vec<f64, A> x, Vec<f64, A> y) noexcept {
        Interpolation ret;
        ret.x_ = move(x);
        ret.y_ = move(y);
        ret.method_ = Interp_Method::Linear;
        return ret;
    }

    [[nodiscard]] static Interpolation log_linear(Vec<f64, A> x, Vec<f64, A> y) noexcept {
        Interpolation ret;
        ret.x_ = move(x);
        ret.y_ = move(y);
        ret.method_ = Interp_Method::LogLinear;
        return ret;
    }

    [[nodiscard]] static Interpolation cubic_spline(Vec<f64, A> x, Vec<f64, A> y) noexcept {
        Interpolation ret;
        u64 n = x.length();
        ret.x_ = move(x);
        ret.y_ = move(y);
        ret.method_ = Interp_Method::CubicSpline;
        ret.impl_ = Vec<f64, A>::make(n);

        if (n < 3) {
            // Fall back to linear for too few points
            ret.method_ = Interp_Method::Linear;
            if (n == 2) {
                ret.impl_[0] = 0.0;
                ret.impl_[1] = 0.0;
            }
            return ret;
        }

        // Natural cubic spline: solve tridiagonal system for second derivatives
        // M = [m_0, m_1, ..., m_{n-1}], with m_0 = m_{n-1} = 0 (natural BC)
        // Interior equations (i=1..n-2):
        //   h_{i-1}*m_{i-1} + 2*(h_{i-1}+h_i)*m_i + h_i*m_{i+1} = 6*(d_i - d_{i-1})
        // where h_i = x_{i+1} - x_i, d_i = (y_{i+1} - y_i) / h_i

        Vec<f64, A> h = Vec<f64, A>::make(n - 1);
        Vec<f64, A> d = Vec<f64, A>::make(n - 1);
        for (u64 i = 0; i < n - 1; i++) {
            h[i] = ret.x_[i + 1] - ret.x_[i];
            d[i] = (ret.y_[i + 1] - ret.y_[i]) / h[i];
        }

        // Tridiagonal system: a_i, b_i, c_i, r_i for i = 1..n-2
        Vec<f64, A> diag = Vec<f64, A>::make(n);     // b: diagonal
        Vec<f64, A> upper = Vec<f64, A>::make(n - 1); // c: upper
        Vec<f64, A> rhs = Vec<f64, A>::make(n);      // right-hand side

        for (u64 i = 1; i < n - 1; i++) {
            f64 h_im1 = h[i - 1];
            f64 h_i = h[i];
            diag[i] = 2.0 * (h_im1 + h_i);
            upper[i] = h_i;
            rhs[i] = 6.0 * (d[i] - d[i - 1]);
        }

        // Thomas algorithm (tridiagonal solver)
        // Forward sweep
        for (u64 i = 2; i < n - 1; i++) {
            f64 h_im1 = h[i - 2];
            f64 piv = h_im1 / diag[i - 1];
            diag[i] -= piv * upper[i - 1];
            rhs[i] -= piv * rhs[i - 1];
        }

        // Back substitution
        ret.impl_[n - 1] = 0.0;  // natural BC
        ret.impl_[0] = 0.0;
        if (n > 2) {
            ret.impl_[n - 2] = rhs[n - 2] / diag[n - 2];
            for (u64 i_i = 3; i_i <= n - 1; i_i++) {
                u64 i = n - i_i;  // n-3 down to 1
                ret.impl_[i] = (rhs[i] - upper[i] * ret.impl_[i + 1]) / diag[i];
            }
        }

        return ret;
    }

    // Fritsch-Carlson monotonic cubic spline (1980)
    [[nodiscard]] static Interpolation monotonic_cubic(Vec<f64, A> x, Vec<f64, A> y) noexcept {
        Interpolation ret;
        u64 n = x.length();
        ret.x_ = move(x);
        ret.y_ = move(y);
        ret.method_ = Interp_Method::MonotonicCubic;
        ret.impl_ = Vec<f64, A>::make(n);

        if (n < 3) {
            ret.method_ = Interp_Method::Linear;
            if (n == 2) {
                ret.impl_[0] = 0.0;
                ret.impl_[1] = 0.0;
            }
            return ret;
        }

        // Step 1: Compute secant slopes d_i
        Vec<f64, A> delta = Vec<f64, A>::make(n - 1);
        Vec<f64, A> h = Vec<f64, A>::make(n - 1);
        for (u64 i = 0; i < n - 1; i++) {
            h[i] = ret.x_[i + 1] - ret.x_[i];
            delta[i] = (ret.y_[i + 1] - ret.y_[i]) / h[i];
        }

        // Step 2: Initialize derivatives at interior points
        Vec<f64, A> m = Vec<f64, A>::make(n);
        // Endpoints: use one-sided slopes
        if (n >= 2) {
            m[0] = delta[0];
            m[n - 1] = delta[n - 2];
        }

        // Interior: weighted harmonic mean of adjacent slopes when signs agree
        for (u64 i = 1; i < n - 1; i++) {
            if (delta[i - 1] * delta[i] <= 0.0) {
                m[i] = 0.0;  // local extremum
            } else {
                // Weighted harmonic mean (Fritsch-Butland variant)
                f64 w1 = 2.0 * h[i] + h[i - 1];
                f64 w2 = h[i] + 2.0 * h[i - 1];
                m[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
            }
        }

        // Step 3: Enforce monotonicity (Fritsch-Carlson constraint)
        for (u64 i = 0; i < n - 1; i++) {
            if (Math::abs(delta[i]) < 1e-15) {
                m[i] = 0.0;
                m[i + 1] = 0.0;
            } else {
                f64 alpha = m[i] / delta[i];
                f64 beta = m[i + 1] / delta[i];

                if (alpha < 0.0) {
                    m[i] = 0.0;
                    alpha = 0.0;
                }
                if (beta < 0.0) {
                    m[i + 1] = 0.0;
                    beta = 0.0;
                }

                // Constraint: alpha^2 + beta^2 <= 9
                f64 dist2 = alpha * alpha + beta * beta;
                if (dist2 > 9.0) {
                    f64 tau = 3.0 / Math::sqrt(dist2);
                    m[i] *= tau;
                    m[i + 1] *= tau;
                }
            }
        }

        // Store first derivatives as the impl_ vector (same semantics as second derivs
        // for Hermite interpolation — we can reconstruct cubics from endpoint values
        // and derivatives)
        ret.impl_ = move(m);

        return ret;
    }

    [[nodiscard]] static Interpolation flat_forward(Vec<f64, A> x, Vec<f64, A> y) noexcept {
        Interpolation ret;
        ret.x_ = move(x);
        ret.y_ = move(y);
        ret.method_ = Interp_Method::FlatForward;
        return ret;
    }

    // ============================================================
    // Binary search: find segment index i such that x_[i] <= x < x_[i+1]
    // ============================================================
    [[nodiscard]] u64 locate(f64 x) const noexcept {
        u64 n = x_.length();
        if (n == 0) return 0;
        if (x <= x_[0]) return 0;
        if (x >= x_[n - 1]) return n - 1;

        u64 lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            u64 mid = (lo + hi) / 2;
            if (x_[mid] <= x) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // ============================================================
    // Interpolate at x (extrapolate flat beyond bounds)
    // ============================================================
    [[nodiscard]] f64 operator()(f64 x) const noexcept {
        u64 n = x_.length();
        if (n == 0) return 0.0;
        if (n == 1) return y_[0];
        if (n == 2) {
            // Linear interpolation for 2 points regardless of method
            if (x <= x_[0]) return y_[0];
            if (x >= x_[1]) return y_[1];
            f64 t = (x - x_[0]) / (x_[1] - x_[0]);
            return y_[0] + t * (y_[1] - y_[0]);
        }

        // Extrapolation: flat beyond bounds
        if (x <= x_[0]) return y_[0];
        if (x >= x_[n - 1]) return y_[n - 1];

        u64 i = locate(x);

        switch (method_) {
        case Interp_Method::Linear: {
            f64 h = x_[i + 1] - x_[i];
            f64 t = (x - x_[i]) / h;
            return y_[i] + t * (y_[i + 1] - y_[i]);
        }

        case Interp_Method::LogLinear: {
            // Interpolate log(y) linearly, then exponentiate
            if (y_[i] <= 0.0 || y_[i + 1] <= 0.0) {
                // Fall back to linear if y values are non-positive
                f64 h = x_[i + 1] - x_[i];
                f64 t = (x - x_[i]) / h;
                return y_[i] + t * (y_[i + 1] - y_[i]);
            }
            f64 h = x_[i + 1] - x_[i];
            f64 t = (x - x_[i]) / h;
            f64 log_val = ::log(y_[i]) + t * (::log(y_[i + 1]) - ::log(y_[i]));
            return Math::exp(log_val);
        }

        case Interp_Method::CubicSpline: {
            // S_i(x) = a_i + b_i*(x-x_i) + c_i*(x-x_i)^2 + d_i*(x-x_i)^3
            f64 h = x_[i + 1] - x_[i];
            f64 dx = x - x_[i];
            f64 a = y_[i];
            f64 c = impl_[i];
            f64 d = (impl_[i + 1] - impl_[i]) / (6.0 * h);
            f64 b = (y_[i + 1] - y_[i]) / h - h * (2.0 * impl_[i] + impl_[i + 1]) / 6.0;

            return a + b * dx + c * dx * dx + d * dx * dx * dx;
        }

        case Interp_Method::MonotonicCubic: {
            // Hermite cubic: given y_i, y_{i+1}, m_i, m_{i+1} (first derivatives)
            f64 h = x_[i + 1] - x_[i];
            f64 dx = x - x_[i];
            f64 t = dx / h;

            // Hermite basis functions
            f64 h00 = (1.0 + 2.0 * t) * (1.0 - t) * (1.0 - t);
            f64 h10 = t * (1.0 - t) * (1.0 - t) * h;
            f64 h01 = t * t * (3.0 - 2.0 * t);
            f64 h11 = t * t * (t - 1.0) * h;

            return h00 * y_[i] + h10 * impl_[i] + h01 * y_[i + 1] + h11 * impl_[i + 1];
        }

        case Interp_Method::FlatForward:
        default: {
            return y_[i];
        }
        }
    }

    // ============================================================
    // First derivative at x
    // ============================================================
    [[nodiscard]] f64 derivative(f64 x) const noexcept {
        u64 n = x_.length();
        if (n < 2) return 0.0;

        if (x <= x_[0]) {
            // One-sided derivative at left endpoint
            return (y_[1] - y_[0]) / (x_[1] - x_[0]);
        }
        if (x >= x_[n - 1]) {
            // One-sided derivative at right endpoint
            return (y_[n - 1] - y_[n - 2]) / (x_[n - 1] - x_[n - 2]);
        }

        u64 i = locate(x);

        switch (method_) {
        case Interp_Method::Linear:
        case Interp_Method::LogLinear: {
            return (y_[i + 1] - y_[i]) / (x_[i + 1] - x_[i]);
        }
        case Interp_Method::CubicSpline: {
            f64 h = x_[i + 1] - x_[i];
            f64 dx = x - x_[i];
            f64 b = (y_[i + 1] - y_[i]) / h - h * (2.0 * impl_[i] + impl_[i + 1]) / 6.0;
            f64 c = impl_[i];
            f64 d = (impl_[i + 1] - impl_[i]) / (6.0 * h);
            return b + 2.0 * c * dx + 3.0 * d * dx * dx;
        }
        case Interp_Method::MonotonicCubic: {
            f64 h = x_[i + 1] - x_[i];
            f64 t = (x - x_[i]) / h;
            // Derivative of Hermite cubic
            f64 dh00_dt = -6.0 * t * (1.0 - t);
            f64 dh10_dt = (1.0 - t) * (1.0 - 3.0 * t);
            f64 dh01_dt = 6.0 * t * (1.0 - t);
            f64 dh11_dt = t * (3.0 * t - 2.0);
            return (dh00_dt * y_[i] / h) + (dh10_dt * impl_[i])
                 + (dh01_dt * y_[i + 1] / h) + (dh11_dt * impl_[i + 1]);
        }
        case Interp_Method::FlatForward:
        default: {
            return 0.0;
        }
        }
    }

    // ============================================================
    // Integral from x_[0] to x
    // ============================================================
    [[nodiscard]] f64 primitive(f64 x) const noexcept {
        u64 n = x_.length();
        if (n == 0) return 0.0;
        if (x <= x_[0]) return 0.0;

        f64 total = 0.0;

        // Sum over complete intervals up to the segment containing x
        u64 end_i = locate(x);
        if (end_i >= n - 1) end_i = n - 2;  // Clamp

        for (u64 i = 0; i < end_i; i++) {
            f64 h = x_[i + 1] - x_[i];
            switch (method_) {
            case Interp_Method::Linear:
            case Interp_Method::LogLinear:
                total += 0.5 * h * (y_[i] + y_[i + 1]);
                break;
            case Interp_Method::CubicSpline: {
                f64 b = (y_[i + 1] - y_[i]) / h - h * (2.0 * impl_[i] + impl_[i + 1]) / 6.0;
                total += y_[i] * h + 0.5 * b * h * h + impl_[i] * h * h * h / 6.0
                       + (impl_[i + 1] - impl_[i]) * h * h * h * h / (24.0 * h);
                break;
            }
            case Interp_Method::MonotonicCubic: {
                f64 h_i = h;
                // Integral of Hermite cubic over [0, h]:
                // int_0^h H(t) dt = 0.5*h*(y_i + y_{i+1}) + h^2/12*(m_i - m_{i+1})
                total += 0.5 * h_i * (y_[i] + y_[i + 1])
                       + h_i * h_i / 12.0 * (impl_[i] - impl_[i + 1]);
                break;
            }
            case Interp_Method::FlatForward:
            default:
                total += h * y_[i];
                break;
            }
        }

        // Add partial interval for the last segment
        if (x > x_[end_i] && end_i < n - 1) {
            f64 h = x_[end_i + 1] - x_[end_i];
            f64 dx = x - x_[end_i];

            switch (method_) {
            case Interp_Method::Linear:
            case Interp_Method::LogLinear: {
                f64 t = dx / h;
                total += dx * (y_[end_i] + 0.5 * t * (y_[end_i + 1] - y_[end_i]));
                break;
            }
            case Interp_Method::CubicSpline: {
                f64 a = y_[end_i];
                f64 b = (y_[end_i + 1] - y_[end_i]) / h - h * (2.0 * impl_[end_i] + impl_[end_i + 1]) / 6.0;
                f64 c = impl_[end_i];
                f64 d = (impl_[end_i + 1] - impl_[end_i]) / (6.0 * h);
                total += a * dx + 0.5 * b * dx * dx + c * dx * dx * dx / 3.0 + d * dx * dx * dx * dx / 4.0;
                break;
            }
            case Interp_Method::MonotonicCubic: {
                // Hermite cubic on [0,1] with values y0, y1 and derivatives m0, m1.
                // H(t) = h00(t)*y0 + h10(t)*h*m0 + h01(t)*y1 + h11(t)*h*m1
                // where h00=(1+2t)(1-t)^2, h10=t(1-t)^2, h01=t^2(3-2t), h11=t^2(t-1)
                // Indefinite integrals (from 0 to t), multiplied by h:
                //   I_h00 = h*(t - t^2 + t^3/3)
                //   I_h10 = h^2*(t^2/2 - 2t^3/3 + t^4/4)
                //   I_h01 = h*(t^2 - t^3*2/3 + ... ) wait, re-derive:
                f64 tt = dx / h;
                f64 t2 = tt * tt;
                f64 t3 = t2 * tt;
                // integral of h00(t)*h dt from 0 to tt:
                //   h00 = 1 - 3t^2 + 2t^3, so int = tt - t3 + 0.5*t3*t = wait
                // Standard Hermite integral from 0 to tt:
                // int_0^tt h00(s) ds = tt - tt^2 + tt^3/3  (wrong, let me recompute)
                //
                // h00(t) = (1+2t)(1-t)^2 = (1+2t)(1 - 2t + t^2) = 1 - 2t + t^2 + 2t - 4t^2 + 2t^3
                //        = 1 - 3t^2 + 2t^3
                // int_0^tt h00(t) dt = tt - t3 + t3/2 = tt - t3/2
                // h10(t) = t(1-t)^2 = t(1 - 2t + t^2) = t - 2t^2 + t^3
                // int_0^tt h10(t) dt = tt^2/2 - 2t3/3 + tt^4/4
                // h01(t) = t^2(3-2t) = 3t^2 - 2t^3
                // int_0^tt h01(t) dt = t3 - t3/2 = t3/2  (wait: t3 - t4_ish)
                // int = t3 - t4/2 = t3 - tt^4/2
                // h11(t) = t^2(t-1) = t^3 - t^2
                // int_0^tt h11(t) dt = tt^4/4 - t3/3
                //
                // Scaled by h:
                f64 I_h00_scaled = h * (tt - t3 / 2.0);
                f64 I_h10_scaled = h * h * (t2 / 2.0 - 2.0 * t3 / 3.0 + t2 * t2 / 4.0);
                f64 I_h01_scaled = h * (t3 - t2 * t2 / 2.0);
                f64 I_h11_scaled = h * h * (t2 * t2 / 4.0 - t3 / 3.0);

                total += y_[end_i] * I_h00_scaled + impl_[end_i] * I_h10_scaled
                       + y_[end_i + 1] * I_h01_scaled + impl_[end_i + 1] * I_h11_scaled;
                break;
            }
            case Interp_Method::FlatForward:
            default:
                total += dx * y_[end_i];
                break;
            }
        }

        return total;
    }
};

} // namespace spp::quant::interp
