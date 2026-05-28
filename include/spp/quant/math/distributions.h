#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <cmath>

namespace spp::quant::dist {

// ============================================================
// Normal distribution
// ============================================================

// Standard normal PDF: phi(x) = exp(-x^2/2) / sqrt(2*pi)
[[nodiscard]] inline f64 normal_pdf(f64 x, f64 mu = 0.0, f64 sigma = 1.0) noexcept {
    f64 z = (x - mu) / sigma;
    return Math::exp(-0.5 * z * z) / (Math::sqrt(2.0 * Math::PI64) * sigma);
}

// Standard normal CDF via Abramowitz & Stegun 26.2.17 rational approximation.
// Horner form for stability. Accuracy ~7.5e-8.
[[nodiscard]] inline f64 normal_cdf(f64 x, f64 mu = 0.0, f64 sigma = 1.0) noexcept {
    f64 z = (x - mu) / sigma;
    if (z < -37.0) return 0.0;
    if (z > 37.0) return 1.0;

    // Use symmetry: Phi(z) = 1 - Phi(-z) for z < 0
    f64 abs_z = z < 0.0 ? -z : z;

    constexpr f64 p = 0.2316419;
    constexpr f64 b1 = 0.319381530;
    constexpr f64 b2 = -0.356563782;
    constexpr f64 b3 = 1.781477937;
    constexpr f64 b4 = -1.821255978;
    constexpr f64 b5 = 1.330274429;

    f64 t = 1.0 / (1.0 + p * abs_z);
    f64 phi = Math::exp(-0.5 * abs_z * abs_z) / Math::sqrt(2.0 * Math::PI64);

    // Horner form: b1*t + b2*t^2 + ... = t*(b1 + t*(b2 + t*(b3 + t*(b4 + t*b5))))
    f64 P = phi * t * (b1 + t * (b2 + t * (b3 + t * (b4 + t * b5))));

    f64 cdf = 1.0 - P;
    return z < 0.0 ? 1.0 - cdf : cdf;
}

// Inverse normal CDF (quantile function) via Peter J. Acklam's algorithm.
// Accuracy: relative error < 1.15e-9 in the entire domain.
// Source: Acklam, P.J. "An algorithm for computing the inverse normal
//         cumulative distribution function."
[[nodiscard]] inline f64 normal_icdf(f64 p, f64 mu = 0.0, f64 sigma = 1.0) noexcept {
    if (p <= 0.0) return -1e300;
    if (p >= 1.0) return 1e300;

    // Constants for central region rational approximation
    constexpr f64 a1 = -3.969683028665376e+01;
    constexpr f64 a2 =  2.209460984245205e+02;
    constexpr f64 a3 = -2.759285104469687e+02;
    constexpr f64 a4 =  1.383577518672690e+02;
    constexpr f64 a5 = -3.066479806614716e+01;
    constexpr f64 a6 =  2.506628277459239e+00;

    constexpr f64 b1 = -5.447609879822406e+01;
    constexpr f64 b2 =  1.615858368580409e+02;
    constexpr f64 b3 = -1.556989798598866e+02;
    constexpr f64 b4 =  6.680131188771972e+01;
    constexpr f64 b5 = -1.328068155288572e+01;

    // Constants for tail region rational approximation
    constexpr f64 c1 = -7.784894002430293e-03;
    constexpr f64 c2 = -3.223964580411365e-01;
    constexpr f64 c3 = -2.400758277161838e+00;
    constexpr f64 c4 = -2.549732539343734e+00;
    constexpr f64 c5 =  4.374664141464968e+00;
    constexpr f64 c6 =  2.938163982698783e+00;

    constexpr f64 d1 =  7.784695709041462e-03;
    constexpr f64 d2 =  3.224671290700398e-01;
    constexpr f64 d3 =  2.445134137142996e+00;
    constexpr f64 d4 =  3.754408661907416e+00;

    // Breakpoint
    constexpr f64 p_low = 0.02425;

    f64 x;

    if (p_low <= p && p <= 1.0 - p_low) {
        // Central region
        f64 q = p - 0.5;
        f64 r = q * q;
        f64 num = q * (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6);
        f64 den = (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
        x = num / den;
    } else {
        // Tail region
        f64 r = (p < 0.5) ? p : 1.0 - p;
        r = ::sqrt(-2.0 * ::log(r));
        f64 num = (((((c1 * r + c2) * r + c3) * r + c4) * r + c5) * r + c6);
        f64 den = ((((d1 * r + d2) * r + d3) * r + d4) * r + 1.0);
        x = num / den;
        if (p >= 0.5) x = -x;
    }

    return mu + sigma * x;
}

// Bivariate normal CDF: Phi2(x, y, rho) via Genz (2004) / Drezner (1978)
// Uses 6-point Gauss-Legendre quadrature over the transformed correlation domain.
// Accuracy ~1e-7 for |rho| < 0.95, degrades near |rho| = 1 but those are
// handled via exact limits.
[[nodiscard]] inline f64 bivariate_normal_cdf(f64 a, f64 b, f64 rho) noexcept {
    // Degenerate cases
    if (rho >= 1.0) return normal_cdf(Math::min(a, b));
    if (rho <= -1.0) return Math::max(0.0, normal_cdf(a) - normal_cdf(-b));
    if (Math::abs(rho) < 1e-15) return normal_cdf(a) * normal_cdf(b);

    // Handle negative correlation by symmetry:
    // Phi2(a, b, rho) = Phi(a) - Phi2(a, -b, -rho)
    if (rho < 0.0) {
        return normal_cdf(a) - bivariate_normal_cdf(a, -b, -rho);
    }

    // For very small rho, still use the formula but be careful
    // 6-point Gauss-Legendre weights and abscissae
    constexpr f64 xg[6] = {
        -0.9324695142031520, -0.6612093864662645, -0.2386191860831969,
         0.2386191860831970,  0.6612093864662645,  0.9324695142031520
    };
    constexpr f64 wg[6] = {
        0.1713244923791703, 0.3607615730481386, 0.4679139345726910,
        0.4679139345726910, 0.3607615730481386, 0.1713244923791703
    };

    // Genz (2004) transform: integrate over s in [0, rho]
    // Phi2(a, b, rho) = Phi(a)Phi(b) + 1/(2pi) * integral from 0 to rho of
    //    exp(-(a^2 - 2*a*b*sin(theta) + b^2)/(2*cos^2(theta))) dtheta
    // where we use the substitution sin(theta) in the integral.
    // After transforming to use Gauss-Legendre on [0, rho]:

    f64 sum = 0.0;
    f64 half_rho = 0.5 * rho;
    f64 a2 = a * a;
    f64 b2 = b * b;
    for (u32 i = 0; i < 6; i++) {
        f64 si = half_rho * (1.0 + xg[i]);  // sin(theta_i), in [0, rho]
        f64 ci = Math::sqrt(1.0 - si * si);  // cos(theta_i)
        f64 exponent = -(a2 + b2 - 2.0 * a * b * si) / (2.0 * ci * ci);
        sum += wg[i] * Math::exp(exponent);
    }
    sum *= half_rho;

    // Ensure result is in [0, 1]
    f64 result = normal_cdf(a) * normal_cdf(b) + sum / (2.0 * Math::PI64);

    // Clamp to valid range
    if (result < 0.0) result = 0.0;
    if (result > 1.0) result = 1.0;
    return result;
}

// ============================================================
// LogNormal distribution
// ============================================================

[[nodiscard]] inline f64 lognormal_pdf(f64 x, f64 mu = 0.0, f64 sigma = 1.0) noexcept {
    if (x <= 0.0) return 0.0;
    f64 log_x = ::log(x);
    f64 z = (log_x - mu) / sigma;
    return Math::exp(-0.5 * z * z) / (Math::sqrt(2.0 * Math::PI64) * sigma * x);
}

[[nodiscard]] inline f64 lognormal_cdf(f64 x, f64 mu = 0.0, f64 sigma = 1.0) noexcept {
    if (x <= 0.0) return 0.0;
    f64 log_x = ::log(x);
    return normal_cdf((log_x - mu) / sigma);
}

// ============================================================
// Internal helpers: regularized incomplete gamma P(a, x)
// ============================================================

namespace detail {

// Series expansion for regularized lower incomplete gamma: P(a, x) = gamma(a,x)/Gamma(a)
// Used when x < a + 1. Converges rapidly.
[[nodiscard]] inline f64 gamma_p_series(f64 a, f64 x) noexcept {
    if (x <= 0.0) return 0.0;

    // ln(Gamma(a)) for the normalizer. Use Stirling or lgamma.
    f64 log_gamma_a = ::lgamma(a);
    f64 ap = a;
    f64 del = 1.0 / a;
    f64 sum = del;
    for (u32 n = 1; n <= 200; n++) {
        ap += 1.0;
        del *= x / ap;
        sum += del;
        if (Math::abs(del) < Math::abs(sum) * 1e-15) break;
    }
    return sum * Math::exp(-x + a * ::log(x) - log_gamma_a);
}

// Continued fraction for regularized upper incomplete gamma: Q(a, x) = Gamma(a,x)/Gamma(a)
// Used for P(a, x) = 1 - Q(a, x) when x >= a + 1.
[[nodiscard]] inline f64 gamma_q_cf(f64 a, f64 x) noexcept {
    // Lentz's method for continued fraction
    f64 log_gamma_a = ::lgamma(a);
    f64 b = x + 1.0 - a;
    f64 c = 1.0 / 1e-30;  // effectively infinite
    f64 d = 1.0 / b;
    f64 h = d;
    for (u32 n = 1; n <= 200; n++) {
        f64 an = -(f64)(n) * (f64(n) - a);
        b += 2.0;
        d = an * d + b;
        if (Math::abs(d) < 1e-30) d = 1e-30;
        c = b + an / c;
        if (Math::abs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        f64 delta = d * c;
        h *= delta;
        if (Math::abs(delta - 1.0) < 1e-15) break;
    }
    return h * Math::exp(-x + a * ::log(x) - log_gamma_a);
}

// Regularized lower incomplete gamma P(a, x)
[[nodiscard]] inline f64 gamma_p(f64 a, f64 x) noexcept {
    if (a <= 0.0) return 0.0;
    if (x <= 0.0) return 0.0;
    if (x < a + 1.0) {
        return gamma_p_series(a, x);
    } else {
        return 1.0 - gamma_q_cf(a, x);
    }
}

// Regularized upper incomplete gamma Q(a, x) = 1 - P(a, x)
[[nodiscard]] inline f64 gamma_q(f64 a, f64 x) noexcept {
    if (a <= 0.0) return 1.0;
    if (x <= 0.0) return 1.0;
    if (x < a + 1.0) {
        return 1.0 - gamma_p_series(a, x);
    } else {
        return gamma_q_cf(a, x);
    }
}

// Inverse regularized lower incomplete gamma P(a, p). Uses Newton iteration
// with gamma_p, seeded by Wilson-Hilferty approximation.
[[nodiscard]] inline f64 gamma_p_inverse(f64 a, f64 p) noexcept {
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1e300;

    // Wilson-Hilferty initial guess:
    // If Y ~ Gamma(a, 1), then Y^{1/3} ~ N(1 - 2/(9a), 2/(9a))
    // So x = a * (1 - 2/(9a) + z * sqrt(2/(9a)))^3
    f64 z = normal_icdf(p);
    f64 t = 1.0 - 2.0 / (9.0 * a) + z * Math::sqrt(2.0 / (9.0 * a));
    t = t * t * t;
    f64 x = a * t;
    if (x < 0.0) x = 1e-10;

    // Newton: x_{n+1} = x_n - (P(a, x_n) - p) / pdf(a, x_n)
    for (u32 iter = 0; iter < 50; iter++) {
        f64 Px = gamma_p(a, x);
        f64 f = Px - p;
        if (Math::abs(f) < 1e-12) break;

        // pdf of Gamma(a,1) at x
        f64 pdf = (a - 1.0) * ::log(x) - x - ::lgamma(a);
        if (x > 0.0 && pdf > -700.0) {
            pdf = Math::exp(pdf);
        } else {
            pdf = 1e-300;
        }
        if (pdf < 1e-300) pdf = 1e-300;

        f64 dx = f / pdf;
        x -= dx;
        if (x <= 0.0) x = 1e-10;
        if (Math::abs(dx) < 1e-10 * x) break;
    }
    return x;
}

// Regularized incomplete beta function I_x(a, b).
// Uses Gauss hypergeometric series when x <= 0.5, symmetry otherwise.
// This is the standard power series: I_x(a,b) = (x^a*(1-x)^b)/(a*B(a,b)) *
//   sum_{n=0}^{inf} (a+b)_n / (a+1)_n * x^n
// where (z)_n = Gamma(z+n)/Gamma(z) is the Pochhammer symbol.
[[nodiscard]] inline f64 incomplete_beta(f64 a, f64 b, f64 x) noexcept {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;

    // Use symmetry: I_x(a, b) = 1 - I_{1-x}(b, a) for better convergence
    if (x > 0.5) {
        return 1.0 - incomplete_beta(b, a, 1.0 - x);
    }

    // Compute log(B(a,b)) for the normalization
    f64 ln_beta = ::lgamma(a) + ::lgamma(b) - ::lgamma(a + b);

    // Series: I_x(a,b) = (x^a * (1-x)^b) * sum_{n=0}^{inf} term_n
    // term_0 = 1/(a * B(a,b))
    // term_{n+1} = term_n * x * (a+b+n) / (a+n+1)

    f64 log_front = a * ::log(x) + b * ::log(1.0 - x) - ln_beta;
    if (log_front < -700.0) return 0.0;

    f64 term = 1.0 / a;
    f64 sum = term;

    constexpr u32 max_iter = 300;
    for (u32 n = 0; n < max_iter; n++) {
        f64 ratio = x * (a + b + (f64)n) / (a + (f64)n + 1.0);
        term *= ratio;
        sum += term;
        if (Math::abs(term) < Math::abs(sum) * 1e-15) break;
    }

    f64 result = Math::exp(log_front) * sum;
    if (result < 0.0) result = 0.0;
    if (result > 1.0) result = 1.0;
    return result;
}

// Inverse regularized incomplete beta function.
// Uses Newton iteration with incomplete_beta.
[[nodiscard]] inline f64 incomplete_beta_inverse(f64 a, f64 b, f64 p) noexcept {
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;

    // Initial guess: use normal approximation
    // For Beta(a,b), approximate mean = a/(a+b), variance = ab/((a+b)^2*(a+b+1))
    // Then use normal quantile to get initial guess
    f64 mean = a / (a + b);
    f64 var = a * b / ((a + b) * (a + b) * (a + b + 1.0));
    f64 sd = Math::sqrt(var);
    f64 z = normal_icdf(p);
    f64 x = mean + z * sd;
    if (x < 1e-6) x = 1e-6;
    if (x > 0.999999) x = 0.999999;

    for (u32 iter = 0; iter < 50; iter++) {
        f64 Ix = incomplete_beta(a, b, x);
        if (Ix < 0.0) Ix = 0.0;
        if (Ix > 1.0) Ix = 1.0;
        f64 f = Ix - p;
        if (Math::abs(f) < 1e-12) break;

        // pdf of Beta(a, b) at x: x^{a-1} * (1-x)^{b-1} / B(a,b)
        f64 ln_beta_ab = ::lgamma(a) + ::lgamma(b) - ::lgamma(a + b);
        f64 log_pdf = (a - 1.0) * ::log(x) + (b - 1.0) * ::log(1.0 - x) - ln_beta_ab;
        f64 pdf = log_pdf < -700.0 ? 1e-300 : Math::exp(log_pdf);
        if (pdf < 1e-300) pdf = 1e-300;

        f64 dx = f / pdf;
        x -= dx;
        if (x <= 0.0) x = 1e-10;
        if (x >= 1.0) x = 0.9999999999;
        if (Math::abs(dx) < 1e-10) break;
    }
    return x;
}

} // namespace detail

// ============================================================
// Chi-square distribution: CDF = P(df/2, x/2)
// ============================================================

[[nodiscard]] inline f64 chi_square_cdf(f64 x, f64 df) noexcept {
    if (x <= 0.0) return 0.0;
    return detail::gamma_p(0.5 * df, 0.5 * x);
}

[[nodiscard]] inline f64 chi_square_icdf(f64 p, f64 df) noexcept {
    // chi^2(df) ~ Gamma(df/2, 2), and gamma inverse of p at alpha=df/2 gives x/2,
    // so x = 2 * gamma_p_inverse(df/2, p)
    return 2.0 * detail::gamma_p_inverse(0.5 * df, p);
}

// ============================================================
// Student's t distribution
// CDF(t, df) = 1 - 0.5 * I_{df/(df+t^2)}(df/2, 1/2)  for t >= 0
// ============================================================

[[nodiscard]] inline f64 student_t_cdf(f64 x, f64 df) noexcept {
    f64 t_abs = x < 0.0 ? -x : x;
    f64 x_in_beta = df / (df + t_abs * t_abs);
    f64 ib = detail::incomplete_beta(0.5 * df, 0.5, x_in_beta);
    f64 tail = 0.5 * ib;
    f64 cdf = 1.0 - tail;
    return x >= 0.0 ? cdf : tail;
}

[[nodiscard]] inline f64 student_t_icdf(f64 p, f64 df) noexcept {
    if (p <= 0.0) return -1e300;
    if (p >= 1.0) return 1e300;

    // For p >= 0.5, work with 1-p and negate
    bool negate = false;
    f64 tail_prob;
    if (p >= 0.5) {
        tail_prob = 2.0 * (1.0 - p);
        negate = true;
    } else {
        tail_prob = 2.0 * p;
    }

    if (tail_prob <= 0.0) {
        return negate ? 0.0 : 1e300;
    }
    if (tail_prob >= 1.0) {
        return negate ? -1e300 : 0.0;
    }

    // x = sqrt(df * (1/BetaInv(tail_prob, df/2, 1/2) - 1))
    f64 beta_val = detail::incomplete_beta_inverse(0.5 * df, 0.5, tail_prob);
    f64 t = Math::sqrt(df * (1.0 / beta_val - 1.0));

    return negate ? t : -t;
}

// ============================================================
// Poisson distribution
// ============================================================

[[nodiscard]] inline f64 poisson_pmf(i32 k, f64 lambda) noexcept {
    if (k < 0) return 0.0;
    if (lambda <= 0.0) return k == 0 ? 1.0 : 0.0;

    // PMF = lambda^k * exp(-lambda) / k!
    // log(PMF) = k * log(lambda) - lambda - log(k!)

    // Compute log(k!) via Stirling asymptotic for large k
    f64 log_k_fact = 0.0;
    if (k > 20) {
        // Stirling: log(k!) ~ k*log(k) - k + 0.5*log(2*pi*k)
        f64 kk = (f64)k;
        log_k_fact = kk * ::log(kk) - kk + 0.5 * ::log(2.0 * Math::PI64 * kk);
        // Add 1/(12k) correction term for better accuracy
        log_k_fact += 1.0 / (12.0 * kk);
    } else {
        // Exact summation for small k
        for (i32 i = 2; i <= k; i++) {
            log_k_fact += ::log((f64)i);
        }
    }

    f64 log_pmf_val = (f64)k * ::log(lambda) - lambda - log_k_fact;
    if (log_pmf_val < -700.0) return 0.0;
    return Math::exp(log_pmf_val);
}

[[nodiscard]] inline f64 poisson_cdf(i32 k, f64 lambda) noexcept {
    if (k < 0) return 0.0;
    if (lambda <= 0.0) return k >= 0 ? 1.0 : 0.0;

    // Direct summation of PMF terms
    f64 sum = 0.0;
    f64 term = Math::exp(-lambda);  // P(0)
    sum = term;

    for (i32 i = 1; i <= k; i++) {
        term *= lambda / (f64)i;
        sum += term;
    }

    if (sum > 1.0) sum = 1.0;
    return sum;
}

} // namespace spp::quant::dist
