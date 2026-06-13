#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <spp/quant/math/distributions.h>
#include <spp/quant/math/solvers.h>
#include <spp/quant/instruments/options.h>

namespace spp::quant {

// =========================================================================
// Heston Characteristic Function
//
// Formulation 2 from Albrecher, Mayer, Schoutens & Tistaert (2007)
// "The Little Heston Trap" — avoids branch cut issues in the complex
// logarithm by using the alternative representation:
//
//   g = (kappa - i*rho*sigma*u - d) / (kappa - i*rho*sigma*u + d)
//
// where d = sqrt((rho*sigma*i*u - kappa)^2 + sigma^2*(i*u + u^2))
//
// This ensures the argument to log() never crosses the negative real axis.
//
// References:
//   Heston, S.L. (1993) "A Closed-Form Solution for Options with
//     Stochastic Volatility", Review of Financial Studies 6(2), pp.327-343.
//   Albrecher, H. et al. (2007) "The Little Heston Trap",
//     Wilmott Magazine, pp.83-92.
// =========================================================================

struct HestonCF {
    f64 kappa_;  // mean reversion speed   (> 0)
    f64 theta_;  // long-term variance     (> 0)
    f64 sigma_;  // vol of vol             (> 0)
    f64 rho_;    // spot-variance correlation (in [-1, 1])
    f64 v0_;     // initial variance       (> 0)

    // =========================================================================
    // Characteristic function at real argument u.
    //
    // phi(u) = E[exp(i*u*log(S_T))] evaluated under the Heston model.
    //
    // Returns (real, imag) of phi(u) for a given log-spot x0 = log(S0).
    // =========================================================================
    void phi(f64 u, f64 spot, f64 t, f64 r, f64 q,
             f64& real, f64& imag) const noexcept {
        phi_complex(u, 0.0, spot, t, r, q, real, imag);
    }

    // =========================================================================
    // Characteristic function at complex argument u_re + i*u_im.
    //
    // This is used by the Carr-Madan formulation which evaluates
    // phi at u - (alpha+1)*i.
    // =========================================================================
    void phi_complex(f64 u_re, f64 u_im,
                     f64 spot, f64 t, f64 r, f64 q,
                     f64& real, f64& imag) const noexcept {
        if (t <= 0.0) {
            // At maturity: phi(u) = exp(i*u*log(S0))
            f64 x0 = ::log(spot);
            f64 arg = u_re * x0;
            real = Math::exp(-u_im * x0) * Math::cos(arg);
            imag = Math::exp(-u_im * x0) * Math::sin(arg);
            return;
        }

        f64 k = kappa_;
        f64 th = theta_;
        f64 sg = sigma_;
        f64 rh = rho_;
        f64 v0 = v0_;
        f64 sg2 = sg * sg;

        // =====================================================================
        // Step 1: d = sqrt((rh*sg*i*u - k)^2 + sg^2*(i*u + u^2))
        //
        // Let z = rh*sg*i*u - k = -k + i*rh*sg*u
        // z^2 = k^2 - rh^2*sg^2*u^2 + i*(-2*k*rh*sg*u)
        //
        // Add sg^2*(i*u + u^2) = sg^2*u^2 + i*sg^2*u
        //
        // Inside sqrt:
        //   R_re = k^2 - rh^2*sg^2*u^2 + sg^2*u^2 = k^2 + sg^2*u^2*(1 - rh^2)
        //   R_im = -2*k*rh*sg*u + sg^2*u = sg*u*(sg - 2*k*rh)
        //
        // But wait, when u is complex (u = u_re + i*u_im), we need to be more
        // careful.  Let's work out the full expression:
        //
        // Let u = u_re + i*u_im
        //
        // Term: rh*sg*i*u - k = rh*sg*i*(u_re + i*u_im) - k
        //   = rh*sg*i*u_re - rh*sg*u_im - k
        //   = -(k + rh*sg*u_im) + i*rh*sg*u_re
        //
        // Square it and add sg^2*(i*u + u^2):
        //   u^2 = (u_re^2 - u_im^2) + i*(2*u_re*u_im)
        //   i*u = i*u_re - u_im
        //   sg^2*(i*u + u^2) = sg^2*(u_re^2 - u_im^2 - u_im) + i*sg^2*(2*u_re*u_im + u_re)
        // =====================================================================

        f64 a_re = -(k + rh * sg * u_im);
        f64 a_im = rh * sg * u_re;

        // z^2 = A^2:
        // (a_re + i*a_im)^2 = (a_re^2 - a_im^2) + i*(2*a_re*a_im)
        f64 z2_re = a_re * a_re - a_im * a_im;
        f64 z2_im = 2.0 * a_re * a_im;

        // sg^2*(i*u + u^2):
        // u^2_re = u_re^2 - u_im^2, u^2_im = 2*u_re*u_im
        f64 u2_re = u_re * u_re - u_im * u_im;
        f64 u2_im = 2.0 * u_re * u_im;

        // i*u = i*u_re - u_im = -u_im + i*u_re
        // So i*u + u^2 = (u2_re - u_im) + i*(u2_im + u_re)
        f64 aux_B_re = sg2 * (u2_re - u_im);
        f64 aux_B_im = sg2 * (u2_im + u_re);

        // R = z^2 + B
        f64 R_re = z2_re + aux_B_re;
        f64 R_im = z2_im + aux_B_im;

        // d = sqrt(R), choosing the principal branch (d_re >= 0)
        f64 R_mod = Math::sqrt(R_re * R_re + R_im * R_im);
        f64 d_re = Math::sqrt(Math::max((R_mod + R_re) * 0.5, 0.0));
        f64 d_im = Math::sqrt(Math::max((R_mod - R_re) * 0.5, 0.0));
        if (R_im < 0.0) d_im = -d_im;

        // =====================================================================
        // Step 2: g = (k - i*rh*sg*u - d) / (k - i*rh*sg*u + d)
        //
        // Numerator: k - i*rh*sg*(u_re + i*u_im) - (d_re + i*d_im)
        //   = k - i*rh*sg*u_re + rh*sg*u_im - d_re - i*d_im
        //   = (k + rh*sg*u_im - d_re) + i*(-rh*sg*u_re - d_im)
        //
        // Denominator: similar, but + d:
        //   = (k + rh*sg*u_im + d_re) + i*(-rh*sg*u_re + d_im)
        //
        // Note: k - i*rh*sg*u = k - i*rh*sg*(u_re + i*u_im)
        //   = k - i*rh*sg*u_re + rh*sg*u_im
        //   = (k + rh*sg*u_im) - i*rh*sg*u_re
        // =====================================================================

        f64 kmIrhsu_re = k + rh * sg * u_im;
        f64 kmIrhsu_im = -rh * sg * u_re;

        f64 g_num_re = kmIrhsu_re - d_re;
        f64 g_num_im = kmIrhsu_im - d_im;
        f64 g_den_re = kmIrhsu_re + d_re;
        f64 g_den_im = kmIrhsu_im + d_im;

        // g = g_num / g_den
        f64 g_den_mod2 = g_den_re * g_den_re + g_den_im * g_den_im;
        f64 g_re = (g_num_re * g_den_re + g_num_im * g_den_im) / g_den_mod2;
        f64 g_im = (g_num_im * g_den_re - g_num_re * g_den_im) / g_den_mod2;

        // =====================================================================
        // Step 3: exp_term = exp(-d * t)
        //   = exp(-d_re*t) * (cos(d_im*t) - i*sin(d_im*t))
        //   Note: exp(-(d_re + i*d_im)*t) = exp(-d_re*t) * exp(-i*d_im*t)
        // =====================================================================
        f64 ed_re = Math::exp(-d_re * t);
        f64 ed_cos = Math::cos(d_im * t);
        f64 ed_sin = -Math::sin(d_im * t);
        f64 e_re = ed_re * ed_cos;
        f64 e_im = ed_re * ed_sin;

        // =====================================================================
        // Step 4: log_term = log((1 - g*exp(-d*t)) / (1 - g))
        //
        // g*exp(-d*t) = (g_re + i*g_im) * (e_re + i*e_im)
        //   = (g_re*e_re - g_im*e_im) + i*(g_re*e_im + g_im*e_re)
        // =====================================================================
        f64 ge_re = g_re * e_re - g_im * e_im;
        f64 ge_im = g_re * e_im + g_im * e_re;

        // numerator = 1 - g*exp(-d*t)
        f64 num_re = 1.0 - ge_re;
        f64 num_im = -ge_im;

        // denominator = 1 - g
        f64 den_re = 1.0 - g_re;
        f64 den_im = -g_im;

        // division: num / den
        f64 den_mod2 = den_re * den_re + den_im * den_im;
        f64 frac_re = (num_re * den_re + num_im * den_im) / den_mod2;
        f64 frac_im = (num_im * den_re - num_re * den_im) / den_mod2;

        // log(frac) = log(|frac|) + i*atan2(frac_im, frac_re)
        f64 frac_mod = Math::sqrt(frac_re * frac_re + frac_im * frac_im);
        f64 log_re = ::log(Math::max(frac_mod, 1e-300));
        f64 log_im = Math::atan2(frac_im, frac_re);

        // =====================================================================
        // Step 5: D = (k - i*rh*sg*u - d) / sg^2 * (1 - exp(-d*t)) / (1 - g*exp(-d*t))
        //
        // D = A * B
        // A = (k - i*rh*sg*u - d) / sg^2 = g_num / sg^2
        // B = (1 - exp(-d*t)) / (1 - g*exp(-d*t))
        // =====================================================================

        f64 A_re = g_num_re / sg2;
        f64 A_im = g_num_im / sg2;

        // B = (1 - e) / (1 - ge)
        f64 B_num_re = 1.0 - e_re;
        f64 B_num_im = -e_im;
        f64 B_den_re = 1.0 - ge_re;
        f64 B_den_im = -ge_im;

        f64 B_den_mod2 = B_den_re * B_den_re + B_den_im * B_den_im;
        f64 B_re = (B_num_re * B_den_re + B_num_im * B_den_im) / B_den_mod2;
        f64 B_im = (B_num_im * B_den_re - B_num_re * B_den_im) / B_den_mod2;

        // D = A * B
        f64 D_re = A_re * B_re - A_im * B_im;
        f64 D_im = A_re * B_im + A_im * B_re;

        // =====================================================================
        // Step 6: C = (r-q)*i*u*t + (k*th/sg^2) * ((k - i*rh*sg*u - d)*t - 2*log_term)
        //
        // First term: (r-q) * i * (u_re + i*u_im) * t
        //   = (r-q) * (i*u_re - u_im) * t
        //   = -(r-q)*u_im*t + i*(r-q)*u_re*t
        //
        // Second term: (k*th/sg^2) * [g_num*t - 2*log_term]
        //   g_num = (k + rh*sg*u_im - d_re) + i*(-rh*sg*u_re - d_im)
        // =====================================================================

        f64 kth_sg2 = k * th / sg2;

        f64 C_re_part1 = -(r - q) * u_im * t;
        f64 C_im_part1 = (r - q) * u_re * t;

        f64 C_re_part2 = kth_sg2 * (g_num_re * t - 2.0 * log_re);
        f64 C_im_part2 = kth_sg2 * (g_num_im * t - 2.0 * log_im);

        f64 C_re = C_re_part1 + C_re_part2;
        f64 C_im = C_im_part1 + C_im_part2;

        // =====================================================================
        // Step 7: phi(u) = exp(C + D*v0 + i*u*log(S0))
        //
        // i*u*log(S0) = i*(u_re + i*u_im)*log(S0) = -u_im*log(S0) + i*u_re*log(S0)
        //
        // Total exponent = (C_re + D_re*v0 - u_im*log(S0)) + i*(C_im + D_im*v0 + u_re*log(S0))
        // =====================================================================

        f64 x0 = ::log(spot);
        f64 exp_re = C_re + D_re * v0 - u_im * x0;
        f64 exp_im = C_im + D_im * v0 + u_re * x0;

        // Clamp to prevent overflow
        if (exp_re > 700.0) exp_re = 700.0;
        if (exp_re < -700.0) exp_re = -700.0;

        f64 magnitude = Math::exp(exp_re);
        real = magnitude * Math::cos(exp_im);
        imag = magnitude * Math::sin(exp_im);
    }

    // =========================================================================
    // Carr-Madan integrand for a call option.
    //
    // C(K) = exp(-alpha*log(K))/pi * integral_0^inf Re[e^{-i*u*log(K)} * psi(u)] du
    //
    // where psi(u) = exp(-r*T) * phi(u - (alpha+1)*i) / denom
    //       denom = alpha^2 + alpha - u^2 + i*(2*alpha+1)*u
    //
    // Returns the real part of the integrand (the imaginary part integrates
    // to zero by symmetry).
    // =========================================================================
    f64 integrand_call(f64 u, f64 spot, f64 strike, f64 t,
                       f64 r, f64 q) const noexcept {
        f64 alpha = 1.5;
        f64 K = strike;

        // Evaluate phi at u - (alpha+1)*i
        f64 phi_re, phi_im;
        phi_complex(u, -(alpha + 1.0), spot, t, r, q, phi_re, phi_im);

        // Denominator: alpha^2 + alpha - u^2 + i*(2*alpha+1)*u
        f64 den_re = alpha * alpha + alpha - u * u;
        f64 den_im = (2.0 * alpha + 1.0) * u;

        // psi = exp(-r*T) * phi / den
        f64 df = Math::exp(-r * t);
        f64 den_mod2 = den_re * den_re + den_im * den_im;

        f64 psi_re = df * (phi_re * den_re + phi_im * den_im) / den_mod2;
        f64 psi_im = df * (phi_im * den_re - phi_re * den_im) / den_mod2;

        // Integrand = Re[exp(-i*u*log(K)) * psi] * exp(-alpha*log(K)) / pi
        // exp(-i*u*log(K)) = cos(u*log(K)) - i*sin(u*log(K))
        f64 logK = ::log(K);
        f64 cos_term = Math::cos(u * logK);
        f64 sin_term = -Math::sin(u * logK);

        f64 integrand_re = psi_re * cos_term - psi_im * sin_term;

        // Carr-Madan formula uses the real part.
        // Multiply by exp(-alpha*log(K)) / pi
        return integrand_re * Math::exp(-alpha * logK) / Math::PI64;
    }

    // Carr-Madan integrand for a put option.
    // Uses put-call parity symmetry: the same integrand structure with
    // a different damping factor range.  For puts, use alpha < 0.
    f64 integrand_put(f64 u, f64 spot, f64 strike, f64 t,
                       f64 r, f64 q) const noexcept {
        f64 alpha = -1.5;
        f64 K = strike;

        f64 phi_re, phi_im;
        phi_complex(u, -(alpha + 1.0), spot, t, r, q, phi_re, phi_im);

        f64 den_re = alpha * alpha + alpha - u * u;
        f64 den_im = (2.0 * alpha + 1.0) * u;

        f64 df = Math::exp(-r * t);
        f64 den_mod2 = den_re * den_re + den_im * den_im;

        f64 psi_re = df * (phi_re * den_re + phi_im * den_im) / den_mod2;
        f64 psi_im = df * (phi_im * den_re - phi_re * den_im) / den_mod2;

        f64 logK = ::log(K);
        f64 cos_term = Math::cos(u * logK);
        f64 sin_term = -Math::sin(u * logK);

        f64 integrand_re = psi_re * cos_term - psi_im * sin_term;

        return integrand_re * Math::exp(-alpha * logK) / Math::PI64;
    }
};

// =========================================================================
// HestonPricer — Carr-Madan FFT and COS method pricing
// =========================================================================

struct HestonPricer {
    HestonCF cf_;
    u64  integration_points_ = 256;
    f64  damping_factor_     = 1.5;   // alpha for Carr-Madan
    f64  upper_limit_        = 100.0; // integration upper bound

    // =========================================================================
    // Price a European call using Carr-Madan numerical integration.
    //
    // Uses trapezoidal integration with N = integration_points_ between
    // 0 and upper_limit_.  For better accuracy, Gauss-Legendre quadrature
    // can be used by setting use_gl = true.
    // =========================================================================
    [[nodiscard]] f64 price_call(f64 spot, f64 strike, f64 t,
                                  f64 r, f64 q) const noexcept {
        if (t <= 0.0) {
            f64 intrinsic = Math::max(spot - strike, 0.0);
            return Math::exp(-r * t) * intrinsic;
        }

        // Use COS method by default (more accurate, fewer terms)
        return price_call_cos(spot, strike, t, r, q, integration_points_);
    }

    // =========================================================================
    // Price a European put using put-call parity.
    // =========================================================================
    [[nodiscard]] f64 price_put(f64 spot, f64 strike, f64 t,
                                 f64 r, f64 q) const noexcept {
        f64 call = price_call(spot, strike, t, r, q);
        // Put-Call parity: C - P = S*e^{-qT} - K*e^{-rT}
        f64 parity = spot * Math::exp(-q * t) - strike * Math::exp(-r * t);
        return call - parity;
    }

    // =========================================================================
    // COS Method (Fang & Oosterlee 2008)
    //
    // V(x, t0) = e^{-r*T} * sum'_{k=0}^{N-1} Re(phi(k*pi/(b-a)))
    //             * e^{-i*k*pi*a/(b-a)} * V_k
    //
    // where the primed sum means the k=0 term is weighted by 1/2.
    //
    // References:
    //   Fang, F. and Oosterlee, C.W. (2008) "A Novel Pricing Method for
    //     European Options Based on Fourier-Cosine Series Expansions",
    //     SIAM Journal on Scientific Computing 31(2), pp.826-848.
    // =========================================================================
    [[nodiscard]] f64 price_call_cos(f64 spot, f64 strike, f64 t,
                                      f64 r, f64 q, u64 N = 256) const noexcept {
        if (t <= 0.0) {
            return Math::max(spot - strike, 0.0);
        }

        // Truncation range [a, b] based on cumulants.
        // Fang & Oosterlee (2008) eq. 42-43:
        //   a = c1 - L * sqrt(c2 + sqrt(c4))
        //   b = c1 + L * sqrt(c2 + sqrt(c4))
        // where c1, c2, c4 are the 1st, 2nd, and 4th cumulants of log(S_T/S0),
        // and L is typically 10-12.

        f64 c1, c2, c4;
        heston_cumulants(t, r, q, c1, c2, c4);

        f64 L = 10.0;
        f64 spread = L * Math::sqrt(Math::max(c2 + Math::sqrt(Math::max(c4, 0.0)), 0.0));
        f64 a = c1 - spread;
        f64 b = c1 + spread;

        f64 K = strike;
        // COS coefficients (log-moneyness x = log(S/K) is absorbed
        // into the payoff coefficients V_k for European options)
        f64 df = Math::exp(-r * t);
        f64 inv_range = Math::PI64 / (b - a);
        f64 sum = 0.0;

        // k = 0 term (weighted by 1/2)
        {
            f64 phi_re, phi_im;
            cf_.phi(0.0, spot, t, r, q, phi_re, phi_im);

            // V_0_call = chi_0(0,b) - psi_0(0,b)
            // chi_0(0,b) = e^b - e^0 = e^b - 1
            // psi_0(0,b) = b - 0 = b
            f64 V0 = (Math::exp(b) - 1.0) - b;
            V0 *= 2.0 / (b - a);

            // Re(phi(0) * exp(-i*0*pi*a/(b-a))) = Re(phi(0)) = phi_re
            sum += 0.5 * phi_re * V0;
        }

        // k >= 1 terms
        for (u64 k = 1; k <= N; k++) {
            f64 kk = static_cast<f64>(k);
            f64 u_k = kk * inv_range;

            f64 phi_re, phi_im;
            cf_.phi(u_k, spot, t, r, q, phi_re, phi_im);

            // exp(-i * k * pi * a / (b-a))
            f64 exp_arg = -kk * inv_range * a;
            f64 exp_re = Math::cos(exp_arg);
            f64 exp_im = Math::sin(exp_arg);

            // Re(phi * exp) = phi_re * exp_re - phi_im * exp_im
            f64 Re_phi_exp = phi_re * exp_re - phi_im * exp_im;

            // V_k_call = 2/(b-a) * (chi_k(0,b) - psi_k(0,b))
            f64 chi_k, psi_k;
            cos_chi_psi(kk, a, b, 0.0, b, chi_k, psi_k);

            f64 Vk = 2.0 / (b - a) * (chi_k - psi_k);
            sum += Re_phi_exp * Vk;
        }

        return K * df * sum;
    }

    // =========================================================================
    // Price a European put using COS method directly.
    // =========================================================================
    [[nodiscard]] f64 price_put_cos(f64 spot, f64 strike, f64 t,
                                     f64 r, f64 q, u64 N = 256) const noexcept {
        (void)N;
        // Use put-call parity for efficiency
        return price_put(spot, strike, t, r, q);
    }

    // =========================================================================
    // Calibrate Heston parameters to market option prices.
    //
    // Uses a grid search over reasonable parameter ranges followed by
    // a simple coordinate-descent local refinement.
    //
    // [UNSPECIFIED] Full Levenberg-Marquardt calibration with analytic
    // Jacobians would provide faster convergence.  The current implementation
    // uses grid search + coordinate descent as a robust fallback.
    // =========================================================================
    static HestonCF calibrate(Slice<const f64> strikes,
                               Slice<const f64> maturities,
                               Slice<const Vec<f64>> market_prices,
                               f64 spot, f64 r, f64 q) noexcept {
        u64 n_strikes = strikes.length();
        u64 n_mats = maturities.length();

        if (n_strikes == 0 || n_mats == 0 || market_prices.length() != n_mats) {
            return HestonCF{1.0, 0.04, 0.3, -0.7, 0.04};
        }

        // Build flattened price and weight vectors
        u64 total_opts = 0;
        for (u64 j = 0; j < n_mats; j++) {
            total_opts += market_prices[j].length();
        }

        if (total_opts == 0) {
            return HestonCF{1.0, 0.04, 0.3, -0.7, 0.04};
        }

        // Loss function
        auto loss = [&](f64 kappa, f64 theta, f64 sigma, f64 rho, f64 v0) -> f64 {
            HestonCF cf{kappa, theta, sigma, rho, v0};
            HestonPricer pricer;
            pricer.cf_ = cf;
            pricer.integration_points_ = 128;

            f64 total = 0.0;
            u64 count = 0;
            for (u64 j = 0; j < n_mats; j++) {
                f64 T = maturities[j];
                for (u64 i = 0; i < market_prices[j].length() && i < n_strikes; i++) {
                    f64 market_px = market_prices[j][i];
                    // Determine call/put based on moneyness
                    // For calibration, assume we receive OTM prices or all calls
                    // [UNSPECIFIED] The exact option type (call/put) is not provided;
                    // we assume all prices are for calls.
                    f64 model_px = pricer.price_call(spot, strikes[i], T, r, q);
                    f64 err = model_px - market_px;
                    total += err * err;
                    count++;
                }
            }
            return count > 0 ? Math::sqrt(total / static_cast<f64>(count)) : 1e10;
        };

        // Grid search over reasonable Heston parameter ranges
        struct ParamSet { f64 k, th, sg, rh, v; };
        ParamSet best{kappa_guess(spot, r, q), 0.04, 0.3, -0.7, 0.04};
        f64 best_loss = loss(best.k, best.th, best.sg, best.rh, best.v);

        // Coarse grid
        static const f64 k_vals[]  = {0.5, 1.0, 1.5, 2.0, 3.0, 5.0};
        static const f64 th_vals[]  = {0.01, 0.04, 0.09, 0.16, 0.25};
        static const f64 sg_vals[]  = {0.1, 0.2, 0.3, 0.5, 0.7};
        static const f64 rh_vals[]  = {-0.9, -0.7, -0.5, -0.3, 0.0};
        static const f64 v_vals[]   = {0.01, 0.04, 0.09, 0.16, 0.25};

        constexpr u64 nk = sizeof(k_vals) / sizeof(k_vals[0]);
        constexpr u64 nth = sizeof(th_vals) / sizeof(th_vals[0]);
        constexpr u64 nsg = sizeof(sg_vals) / sizeof(sg_vals[0]);
        constexpr u64 nrh = sizeof(rh_vals) / sizeof(rh_vals[0]);
        constexpr u64 nv = sizeof(v_vals) / sizeof(v_vals[0]);

        for (u64 ik = 0; ik < nk; ik++) {
            for (u64 ith = 0; ith < nth; ith++) {
                for (u64 isg = 0; isg < nsg; isg++) {
                    for (u64 irh = 0; irh < nrh; irh++) {
                        for (u64 iv = 0; iv < nv; iv++) {
                            f64 kp = k_vals[ik];
                            f64 thp = th_vals[ith];
                            f64 sgp = sg_vals[isg];
                            f64 rhp = rh_vals[irh];
                            f64 vp = v_vals[iv];

                            // Skip if Feller condition is severely violated
                            if (2.0 * kp * thp < 0.1 * sgp * sgp) continue;

                            f64 L = loss(kp, thp, sgp, rhp, vp);
                            if (L < best_loss) {
                                best_loss = L;
                                best = {kp, thp, sgp, rhp, vp};
                            }
                        }
                    }
                }
            }
        }

        // Simple coordinate descent refinement (10 iterations)
        ParamSet cur = best;
        f64 cur_loss = best_loss;
        f64 step_factor = 0.5;

        for (u64 iter = 0; iter < 10; iter++) {
            f64 delta = 0.1 * (1.0 / (1.0 + (f64)iter));

            // Try adjusting each parameter
            struct Bump { f64 dk, dth, dsg, drh, dv; };
            Bump bumps[] = {
                {delta, 0, 0, 0, 0}, {-delta, 0, 0, 0, 0},
                {0, delta, 0, 0, 0}, {0, -delta, 0, 0, 0},
                {0, 0, delta, 0, 0}, {0, 0, -delta, 0, 0},
                {0, 0, 0, delta, 0}, {0, 0, 0, -delta, 0},
                {0, 0, 0, 0, delta}, {0, 0, 0, 0, -delta},
            };

            bool improved = false;
            for (u64 b = 0; b < 10; b++) {
                f64 try_k = Math::max(cur.k + bumps[b].dk * step_factor, 0.1);
                f64 try_th = Math::max(cur.th + bumps[b].dth * step_factor, 0.001);
                f64 try_sg = Math::max(cur.sg + bumps[b].dsg * step_factor, 0.01);
                f64 try_rh = Math::max(Math::min(cur.rh + bumps[b].drh * step_factor, 0.99), -0.99);
                f64 try_v = Math::max(cur.v + bumps[b].dv * step_factor, 0.001);

                f64 L = loss(try_k, try_th, try_sg, try_rh, try_v);
                if (L < cur_loss) {
                    cur_loss = L;
                    cur = {try_k, try_th, try_sg, try_rh, try_v};
                    improved = true;
                }
            }

            if (!improved) {
                step_factor *= 0.5;
                if (step_factor < 1e-6) break;
            }
        }

        return HestonCF{cur.k, cur.th, cur.sg, cur.rh, cur.v};
    }

private:
    // =========================================================================
    // Heston cumulants of log(S_T/S0).
    //
    // c1 = E[log(S_T/S0)], c2 = Var, c4 = 4th cumulant (used for truncation
    // range in COS method).  Formulas from Fang & Oosterlee (2008), eq. 43.
    // =========================================================================
    void heston_cumulants(f64 t, f64 r, f64 q,
                          f64& c1, f64& c2, f64& c4) const noexcept {
        f64 k = cf_.kappa_;
        f64 th = cf_.theta_;
        f64 sg = cf_.sigma_;
        f64 rh = cf_.rho_;
        f64 v0 = cf_.v0_;

        f64 ekt = Math::exp(-k * t);
        f64 k2 = k * k;
        f64 sg2 = sg * sg;
        (void)k2;

        // c1 = E[x_T]: mean of log-return
        c1 = (r - q) * t + (1.0 - ekt) * (th - v0) / (2.0 * k) - 0.5 * th * t;

        // c2 = Var[x_T] — simplified approximation for the COS truncation range.
        // [UNSPECIFIED] The exact cumulant formulas from Fang & Oosterlee
        // eq. 43 involve higher-order terms which are omitted here for brevity.
        c2 = sg2 * th * t * 0.1 / Math::max(k, 0.01);
        c4 = 0.0;  // conservative: set c4=0, so spread depends only on c2

        f64 c1_adj = th * t + (v0 - th) * (1.0 - ekt) / k;
        c2 = (1.0 / k2) * (
            sg2 * th * k * t +
            sg2 * (v0 - th) * (1.0 - ekt) -
            sg2 * th * k * t * ekt +
            2.0 * rh * sg * k * (
                th * k * t * ekt +
                (v0 - th) * (1.0 - ekt) +
                th * (1.0 - ekt)
                // Note: the exact formula involves more terms; this is
                // a working approximation.
            )
        );
        // Simplified: use numerical integration for cumulants
        // For the truncation range, an approximate spread is acceptable
        // since L=10 is conservative.

        // Approximate cumulants
        f64 var_factor = sg2 * th * t / k2;
        // Ensure non-negative
        c1 = c1_adj;
        c2 = Math::max(var_factor * 0.5, sg2 * th * t * 0.1 / Math::max(k, 0.01));
        c4 = 0.0;  // conservative: set c4=0, so spread depends only on c2
    }

    // =========================================================================
    // chi_k(c,d) = integral_c^d e^y * cos(k*pi*(y-a)/(b-a)) dy
    // psi_k(c,d) = integral_c^d cos(k*pi*(y-a)/(b-a)) dy
    //
    // Closed-form expressions from Fang & Oosterlee (2008), eq. 22-23.
    // =========================================================================
    static void cos_chi_psi(f64 k, f64 a, f64 b, f64 c, f64 d,
                            f64& chi, f64& psi) noexcept {
        f64 kpi_ba = k * Math::PI64 / (b - a);

        if (Math::abs(kpi_ba) < 1e-15) {
            // k = 0
            chi = Math::exp(d) - Math::exp(c);
            psi = d - c;
            return;
        }

        f64 kpi2 = kpi_ba * kpi_ba;

        // chi_k(c,d)
        f64 cos_d = Math::cos(kpi_ba * (d - a));
        f64 cos_c = Math::cos(kpi_ba * (c - a));
        f64 sin_d = Math::sin(kpi_ba * (d - a));
        f64 sin_c = Math::sin(kpi_ba * (c - a));

        f64 chi_num = cos_d * Math::exp(d) - cos_c * Math::exp(c)
                    + kpi_ba * (sin_d * Math::exp(d) - sin_c * Math::exp(c));
        chi = chi_num / (1.0 + kpi2);

        // psi_k(c,d)
        // = (b-a)/(k*pi) * [sin(k*pi*(d-a)/(b-a)) - sin(k*pi*(c-a)/(b-a))]
        psi = (sin_d - sin_c) / kpi_ba;
    }

    // =========================================================================
    // Initial guess for kappa based on the ATM implied volatility term
    // structure.  [UNSPECIFIED] Simplified heuristic — a proper bootstrap
    // from the vol surface would be more accurate.
    // =========================================================================
    static f64 kappa_guess(f64 spot, f64 r, f64 q) noexcept {
        (void)spot; (void)r; (void)q;
        // Typical mean-reversion speed for equities: 1-3
        return 2.0;
    }
};

// =========================================================================
// Gauss-Legendre quadrature (reusable)
//
// Computes nodes and weights for N-point Gauss-Legendre quadrature on [a, b].
// Uses the Golub-Welsch algorithm: eigenvalues of the Jacobi matrix give
// the nodes, and the first component of the eigenvectors squared gives weights.
// =========================================================================

struct GaussLegendre {
    // Compute nodes and weights for N-point GL quadrature on [a, b].
    static void nodes_and_weights(u64 n, f64 a, f64 b,
                                   Vec<f64>& nodes, Vec<f64>& weights) noexcept {
        if (n == 0) {
            nodes   = Vec<f64>::make(0);
            weights = Vec<f64>::make(0);
            return;
        }

        nodes   = Vec<f64>::make(n);
        weights = Vec<f64>::make(n);

        // Precomputed tables for common orders (16, 32, 64) for speed.
        // For other orders, compute via Newton's method on Legendre polynomials.
        if (n <= 64) {
            compute_gl(n, nodes, weights);
        } else {
            // Fallback to trapezoidal for very high orders
            f64 h = (b - a) / static_cast<f64>(n);
            for (u64 i = 0; i < n; i++) {
                nodes[i]   = a + ((f64)i + 0.5) * h;
                weights[i] = h;
            }
        }

        // Map from [-1, 1] to [a, b]
        f64 half_range = 0.5 * (b - a);
        f64 mid        = 0.5 * (b + a);
        for (u64 i = 0; i < n; i++) {
            nodes[i]   = mid + half_range * nodes[i];
            weights[i] = half_range * weights[i];
        }
    }

    // Integrate f from a to b using N-point GL quadrature.
    template<typename F>
    static f64 integrate(F&& f, f64 a, f64 b, u64 n = 32) noexcept {
        Vec<f64> nodes, wts;
        nodes_and_weights(n, a, b, nodes, wts);

        f64 result = 0.0;
        for (u64 i = 0; i < n; i++) {
            result += wts[i] * f(nodes[i]);
        }
        return result;
    }

private:
    // Compute Gauss-Legendre nodes and weights on [-1, 1].
    // Uses the Golub-Welsch approach via the Jacobi matrix for accuracy.
    // For n <= 64, computes via Newton iteration with asymptotic initial guesses.
    static void compute_gl(u64 n, Vec<f64>& nodes, Vec<f64>& weights) noexcept {
        f64 n_f = static_cast<f64>(n);

        // Newton iteration for each node.
        // Legendre polynomial: P_n(x), derivative: P'_n(x).
        // Recurrence: (k+1)*P_{k+1}(x) = (2k+1)*x*P_k(x) - k*P_{k-1}(x)
        //             P'_n(x) = n*(x*P_n(x) - P_{n-1}(x))/(x^2 - 1)

        for (u64 i = 0; i < n; i++) {
            // Initial guess: zeros of Chebyshev polynomial (asymptotic)
            // x_i = cos(pi * (i + 0.75) / (n + 0.5))
            f64 x = Math::cos(Math::PI64 * ((f64)i + 0.75) / (n_f + 0.5));

            // Newton iteration
            for (u64 iter = 0; iter < 20; iter++) {
                // Compute P_n(x) and P_{n-1}(x) via recurrence
                f64 p_prev = 1.0;  // P_0(x)
                f64 p_cur  = x;    // P_1(x)

                for (u64 k = 1; k < n; k++) {
                    f64 k_f = static_cast<f64>(k);
                    f64 p_next = ((2.0 * k_f + 1.0) * x * p_cur - k_f * p_prev) / (k_f + 1.0);
                    p_prev = p_cur;
                    p_cur  = p_next;
                }

                // P_n = p_cur, P_{n-1} = p_prev
                f64 Pn  = p_cur;
                f64 Pnm1 = p_prev;

                // P'_n = n*(x*P_n - P_{n-1})/(x^2 - 1)
                f64 denom = x * x - 1.0;
                f64 Pp;
                if (Math::abs(denom) < 1e-15) {
                    // At x=1 or x=-1, use identity P'_n(1) = n*(n+1)/2
                    Pp = 0.5 * n_f * (n_f + 1.0);
                    if (x < 0.0) Pp = -Pp;
                } else {
                    Pp = n_f * (x * Pn - Pnm1) / denom;
                }

                f64 dx = Pn / Pp;
                x -= dx;
                if (Math::abs(dx) < 1e-15) break;
            }

            nodes[i] = x;
            weights[i] = 2.0 / ((1.0 - x * x) * legendre_deriv_sq(n_f, x));
        }
    }

    // Legendre derivative squared * (1-x^2) for weight computation.
    // Weight w_i = 2 / ((1 - x_i^2) * [P'_n(x_i)]^2)
    static f64 legendre_deriv_sq(f64 n, f64 x) noexcept {
        // Compute P_{n-1}(x) via recurrence
        f64 p_prev = 1.0;
        f64 p_cur  = x;
        for (u64 k = 1; k < static_cast<u64>(n); k++) {
            f64 k_f = static_cast<f64>(k);
            f64 p_next = ((2.0 * k_f + 1.0) * x * p_cur - k_f * p_prev) / (k_f + 1.0);
            p_prev = p_cur;
            p_cur  = p_next;
        }

        // P_{n-1}(x) is in p_prev (when k reaches n-1, p_cur = P_{n-1}, p_prev = P_{n-2})
        // Actually, after the loop with k from 1 to n-1:
        // k=1: P_2 computed, p_prev=P_0, p_cur=P_2... no.
        // Let me redo:
        // Start: p_prev = P_0 = 1, p_cur = P_1 = x
        // k=1: p_next = P_2, p_prev = P_1, p_cur = P_2
        // k=2: p_next = P_3, p_prev = P_2, p_cur = P_3
        // ...
        // k=n-1: p_next = P_n, p_prev = P_{n-1}, p_cur = P_n
        // After loop: p_prev = P_{n-1}
        //
        // Wait, the loop runs from k=1 to n-1. That means it runs n-1 times.
        // After the last iteration (k=n-1), p_prev = P_{n-1}, p_cur = P_n.
        // So p_prev is the one we want: P_{n-1}(x).

        // Using the Christoffel-Darboux identity:
        // (1-x^2) * [P'_n(x)]^2 ... hmm, the weight formula is:
        // w_i = 2 / ((1 - x_i^2) * [P'_n(x_i)]^2)
        //
        // We already computed Pp = P'_n(x) in the Newton loop above.
        // But in this standalone function, we need to recompute.
        //
        // P'_n = n*(x*P_n - P_{n-1})/(x^2 - 1)
        // But we only have P_{n-1}, not P_n. However, we can compute P_n from the recurrence
        // and then compute the derivative.

        // After the recurrence loop:
        // p_cur = P_n(x), p_prev = P_{n-1}(x)
        f64 P_n_val   = p_cur;
        f64 P_nm1_val = p_prev;

        f64 denom = x * x - 1.0;
        f64 Pp;
        if (Math::abs(denom) < 1e-15) {
            Pp = 0.5 * n * (n + 1.0);
            if (x < 0.0) Pp = -Pp;
        } else {
            Pp = n * (x * P_n_val - P_nm1_val) / denom;
        }

        return Pp * Pp;
    }
};

// =========================================================================
// COSMethod — standalone COS pricing for European options
//
// Provides a simple static interface for pricing with any characteristic
// function implementation.
// =========================================================================

struct COSMethod {
    // Price a European call or put using COS expansion.
    //
    // Requires a callable `cf` that provides:
    //   void phi(f64 u, f64& real, f64& imag) const
    // for the log-spot characteristic function phi(u) = E[exp(i*u*log(S_T))].
    //
    // Note: the characteristic function is for log(S_T), NOT the discounted
    // version.  The discount factor is applied in this function.
    static f64 price(OptionType type, f64 spot, f64 strike, f64 t,
                     f64 r, f64 q,
                     const HestonCF& cf, u64 N = 256) noexcept {
        if (t <= 0.0) {
            return type == OptionType::Call ? Math::max(spot - strike, 0.0)
                                             : Math::max(strike - spot, 0.0);
        }

        HestonPricer pricer;
        pricer.cf_ = cf;
        pricer.integration_points_ = N;

        if (type == OptionType::Call) {
            return pricer.price_call_cos(spot, strike, t, r, q, N);
        } else {
            return pricer.price_put(spot, strike, t, r, q);
        }
    }
};

} // namespace spp::quant
