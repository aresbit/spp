#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <spp/quant/instruments/options.h>
#include <spp/quant/data/market_data.h>
#include <spp/quant/math/distributions.h>
#include <spp/quant/math/solvers.h>

namespace spp::quant {

// =========================================================================
// BlackScholes — analytic pricing for European, digital, and barrier options
//
// Black, F. and Scholes, M. (1973) "The Pricing of Options and Corporate
// Liabilities", Journal of Political Economy 81(3), pp.637-654.
//
// Merton, R.C. (1973) "Theory of Rational Option Pricing",
// Bell Journal of Economics and Management Science 4(1), pp.141-183.
//
// Barrier formulas: Reiner, E. and Rubinstein, M. (1991)
// "Breaking Down the Barriers", RISK 4(8), pp.28-35.
// =========================================================================

struct BlackScholes {

    // --- d1 / d2 helper ----------------------------------------------------
    static void d1_d2(f64 spot, f64 strike, f64 t, f64 r, f64 q, f64 vol,
                      f64& d1, f64& d2) noexcept {
        f64 vt = vol * Math::sqrt(Math::max(t, 0.0));
        if (vt < 1e-15) {
            // Edge case: zero time or zero vol => d1/d2 diverge
            if (spot > strike) { d1 =  1e15; d2 =  1e15; }
            else               { d1 = -1e15; d2 = -1e15; }
            return;
        }
        d1 = (::log(spot / strike) + (r - q + 0.5 * vol * vol) * t) / vt;
        d2 = d1 - vt;
    }

    // --- Core Black-Scholes price ------------------------------------------
    // C = S*e^{-qT}*N(d1) - K*e^{-rT}*N(d2)
    // P = K*e^{-rT}*N(-d2) - S*e^{-qT}*N(-d1)
    static f64 price(OptionType type, f64 spot, f64 strike, f64 t,
                     f64 r, f64 q, f64 vol) noexcept {
        if (t <= 0.0) {
            // At expiry, intrinsic value
            if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
            else                          return Math::max(strike - spot, 0.0);
        }
        f64 d1, d2;
        d1_d2(spot, strike, t, r, q, vol, d1, d2);

        f64 df_r = Math::exp(-r * t);
        f64 df_q = Math::exp(-q * t);

        if (type == OptionType::Call)
            return spot * df_q * dist::normal_cdf(d1) -
                   strike * df_r * dist::normal_cdf(d2);
        else
            return strike * df_r * dist::normal_cdf(-d2) -
                   spot * df_q * dist::normal_cdf(-d1);
    }

    // Convenience overload using instruments and MarketData
    static f64 price(const EuropeanOption& opt, const MarketData& mkt) noexcept {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = static_cast<f64>(opt.expiry_.serial_ - mkt.as_of_.serial_) / 365.0;
        f64 r = mkt.zero_rate(opt.expiry_, Compounding::Continuous, Frequency::Annual);
        f64 q = mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        return price(opt.type_, S, K, T, r, q, v);
    }

    // --- Full Greeks bundle ------------------------------------------------
    static Greeks greeks(OptionType type, f64 spot, f64 strike, f64 t,
                         f64 r, f64 q, f64 vol) noexcept {
        Greeks g;
        if (t <= 0.0) {
            // At expiry: delta = 1/0 (ITM/OTM), all others zero
            bool itm = (type == OptionType::Call) ? (spot > strike) : (spot < strike);
            g.delta = itm ? 1.0 : 0.0;
            return g;
        }

        f64 d1, d2;
        d1_d2(spot, strike, t, r, q, vol, d1, d2);

        f64 df_q  = Math::exp(-q * t);
        f64 df_r  = Math::exp(-r * t);
        f64 vt    = vol * Math::sqrt(t);
        f64 nd1   = dist::normal_cdf(d1);
        f64 nd2   = dist::normal_cdf(d2);
        f64 nnd1  = dist::normal_cdf(-d1);
        f64 nd1p  = dist::normal_pdf(d1);       // N'(d1) = phi(d1)
        f64 sqt   = Math::sqrt(t);

        if (type == OptionType::Call) {
            // Delta = e^{-qT} * N(d1)
            g.delta = df_q * nd1;
            // Gamma = e^{-qT} * N'(d1) / (S * vol * sqrt(T))
            g.gamma = df_q * nd1p / (spot * vt);
            // Vega = S * e^{-qT} * N'(d1) * sqrt(T)  (per 1% = 0.01 vol)
            g.vega  = spot * df_q * nd1p * sqt * 0.01;
            // Theta = -S*e^{-qT}*N'(d1)*vol/(2*sqrt(T)) - r*K*e^{-rT}*N(d2) + q*S*e^{-qT}*N(d1)
            g.theta = -spot * df_q * nd1p * vol / (2.0 * sqt)
                      - r * strike * df_r * nd2
                      + q * spot * df_q * nd1;
            g.theta /= 365.0; // per calendar day
            // Rho = K * T * e^{-rT} * N(d2)  (per 1% = 0.01 rate)
            g.rho = strike * t * df_r * nd2 * 0.01;
        } else {
            // Put
            g.delta = -df_q * nnd1;
            g.gamma = df_q * nd1p / (spot * vt);  // same as call
            g.vega  = spot * df_q * nd1p * sqt * 0.01; // same as call
            g.theta = -spot * df_q * nd1p * vol / (2.0 * sqt)
                      + r * strike * df_r * dist::normal_cdf(-d2)
                      - q * spot * df_q * nnd1;
            g.theta /= 365.0;
            g.rho = -strike * t * df_r * dist::normal_cdf(-d2) * 0.01;
        }

        return g;
    }

    static Greeks greeks(const EuropeanOption& opt, const MarketData& mkt) noexcept {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = static_cast<f64>(opt.expiry_.serial_ - mkt.as_of_.serial_) / 365.0;
        f64 r = mkt.zero_rate(opt.expiry_, Compounding::Continuous, Frequency::Annual);
        f64 q = mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        return greeks(opt.type_, S, K, T, r, q, v);
    }

    // --- Individual Greeks -------------------------------------------------
    static f64 delta(OptionType type, f64 spot, f64 strike, f64 t,
                     f64 r, f64 q, f64 vol) noexcept {
        return greeks(type, spot, strike, t, r, q, vol).delta;
    }

    static f64 gamma(f64 spot, f64 strike, f64 t,
                     f64 r, f64 q, f64 vol) noexcept {
        if (t <= 0.0) return 0.0;
        f64 d1, d2;
        d1_d2(spot, strike, t, r, q, vol, d1, d2);
        (void)d2;
        f64 vt = vol * Math::sqrt(t);
        return Math::exp(-q * t) * dist::normal_pdf(d1) / (spot * vt);
    }

    static f64 vega(f64 spot, f64 strike, f64 t,
                    f64 r, f64 q, f64 vol) noexcept {
        if (t <= 0.0) return 0.0;
        f64 d1, d2;
        d1_d2(spot, strike, t, r, q, vol, d1, d2);
        (void)d2;
        return spot * Math::exp(-q * t) * dist::normal_pdf(d1) *
               Math::sqrt(t) * 0.01;
    }

    static f64 theta(OptionType type, f64 spot, f64 strike, f64 t,
                     f64 r, f64 q, f64 vol) noexcept {
        return greeks(type, spot, strike, t, r, q, vol).theta;
    }

    static f64 rho(OptionType type, f64 spot, f64 strike, f64 t,
                   f64 r, f64 q, f64 vol) noexcept {
        return greeks(type, spot, strike, t, r, q, vol).rho;
    }

    // --- Implied volatility ------------------------------------------------
    // Uses Brent's method from solvers.h.
    static Opt<f64> implied_vol(OptionType type, f64 target_price, f64 spot,
                                f64 strike, f64 t, f64 r, f64 q) noexcept {
        if (target_price <= 0.0 || t <= 0.0) return {};

        // Intrinsic check: vol=0 price
        f64 intrinsic = Math::max(
            (type == OptionType::Call) ? spot - strike : strike - spot, 0.0);
        f64 disc_intrinsic = intrinsic * Math::exp(-r * t);
        if (target_price <= disc_intrinsic + 1e-12) return Opt{f64{1e-6}};

        auto price_fn = [&](f64 v) -> f64 {
            return price(type, spot, strike, t, r, q, v);
        };

        return solvers::implied_volatility(price_fn, target_price,
                                            1e-6, 10.0, 1e-8);
    }

    // --- Digital (cash-or-nothing) option ----------------------------------
    // Digital call:  cash * e^{-rT} * N(d2)
    // Digital put:   cash * e^{-rT} * N(-d2)
    static f64 digital_price(OptionType type, f64 spot, f64 strike, f64 t,
                             f64 r, f64 q, f64 vol) noexcept {
        if (t <= 0.0) {
            bool itm = (type == OptionType::Call) ? (spot > strike) : (spot < strike);
            return itm ? 1.0 : 0.0;  // per unit cash rebate
        }
        f64 d1, d2;
        d1_d2(spot, strike, t, r, q, vol, d1, d2);
        (void)d1;
        f64 df_r = Math::exp(-r * t);
        if (type == OptionType::Call)
            return df_r * dist::normal_cdf(d2);
        else
            return df_r * dist::normal_cdf(-d2);
    }

    // --- Barrier option (Reiner-Rubinstein 1991) ---------------------------
    // Continuous monitoring, 8 standard types.
    static f64 barrier_price(BarrierOption::BarrierType barrier_type,
                             OptionType type,
                             f64 spot, f64 strike, f64 barrier,
                             f64 t, f64 r, f64 q, f64 vol) noexcept {
        if (t <= 0.0) {
            // At expiry — check if barrier was hit or not
            bool knocked = false;
            switch (barrier_type) {
            case BarrierOption::BarrierType::DownAndOut:
            case BarrierOption::BarrierType::DownAndIn:
                knocked = (spot <= barrier); break;
            case BarrierOption::BarrierType::UpAndOut:
            case BarrierOption::BarrierType::UpAndIn:
                knocked = (spot >= barrier); break;
            }
            bool is_in = (barrier_type == BarrierOption::BarrierType::DownAndIn ||
                          barrier_type == BarrierOption::BarrierType::UpAndIn);
            bool active = is_in ? knocked : !knocked;
            if (!active) return 0.0;
            if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
            else                          return Math::max(strike - spot, 0.0);
        }

        f64 phi = (type == OptionType::Call) ? 1.0 : -1.0;
        bool is_down = (barrier_type == BarrierOption::BarrierType::DownAndOut ||
                        barrier_type == BarrierOption::BarrierType::DownAndIn);
        f64 eta = is_down ? 1.0 : -1.0;

        f64 v   = vol;
        f64 v2  = v * v;
        f64 vt  = v * Math::sqrt(t);
        f64 df_r = Math::exp(-r * t);
        f64 df_q = Math::exp(-q * t);

        // Reiner-Rubinstein parameters
        f64 mu  = (r - q - 0.5 * v2) / v2;
        f64 lambda = Math::sqrt(mu * mu + 2.0 * r / v2);
        f64 h_over_s = barrier / spot;

        // x1, x2
        f64 x1 = ::log(spot / strike) / vt + (1.0 + mu) * vt;
        f64 x2 = ::log(spot / barrier) / vt + (1.0 + mu) * vt;
        // y1, y2
        f64 y1 = ::log((barrier * barrier) / (spot * strike)) / vt +
                 (1.0 + mu) * vt;
        f64 y2 = ::log(barrier / spot) / vt + (1.0 + mu) * vt;
        // z
        f64 z  = ::log(barrier / spot) / vt + lambda * vt;

        // Building blocks A through F
        auto N = [](f64 x) { return dist::normal_cdf(x); };

        f64 A = phi * spot * df_q * N(phi * x1) -
                phi * strike * df_r * N(phi * x1 - phi * vt);

        f64 B = phi * spot * df_q * N(phi * x2) -
                phi * strike * df_r * N(phi * x2 - phi * vt);

        f64 pow1 = Math::pow(h_over_s, 2.0 * (mu + 1.0));
        f64 pow2 = Math::pow(h_over_s, 2.0 * mu);

        f64 C = phi * spot * df_q * pow1 * N(eta * y1) -
                phi * strike * df_r * pow2 * N(eta * y1 - eta * vt);

        f64 D = phi * spot * df_q * pow1 * N(eta * y2) -
                phi * strike * df_r * pow2 * N(eta * y2 - eta * vt);

        f64 E = strike * df_r * (N(eta * x2 - eta * vt) -
                                  pow2 * N(eta * y2 - eta * vt));

        f64 pow3 = Math::pow(h_over_s, mu + lambda);
        f64 pow4 = Math::pow(h_over_s, mu - lambda);

        f64 F = pow3 * N(eta * z) +
                pow4 * N(eta * z - 2.0 * eta * lambda * vt);

        bool x_gt_h = strike > barrier;

        // Select formula based on barrier type, option type, and strike/barrier
        // relationship.  Following Reiner-Rubinstein (1991) / Haug (2007).
        f64 result = 0.0;

        switch (barrier_type) {
        case BarrierOption::BarrierType::DownAndIn:
            if (type == OptionType::Call) {
                result = (x_gt_h) ? (C + E) : (A - B + D + E);
            } else { // Put
                result = (x_gt_h) ? (B - C + D + E) : (A + E);
            }
            break;

        case BarrierOption::BarrierType::DownAndOut:
            if (type == OptionType::Call) {
                result = (x_gt_h) ? (A - C + F) : (B - D + F);
            } else { // Put
                result = (x_gt_h) ? (A - B + C - D + F) : F;
            }
            break;

        case BarrierOption::BarrierType::UpAndIn:
            if (type == OptionType::Call) {
                result = (x_gt_h) ? (A + E) : (B - C + D + E);
            } else { // Put
                result = (x_gt_h) ? (A - B + D + E) : (C + E);
            }
            break;

        case BarrierOption::BarrierType::UpAndOut:
            if (type == OptionType::Call) {
                result = (x_gt_h) ? F : (A - B + C - D + F);
            } else { // Put
                result = (x_gt_h) ? (B - D + F) : (A - C + F);
            }
            break;
        }

        // Clamp — barrier prices can have small numerical noise
        f64 bs_price = price(type, spot, strike, t, r, q, vol);
        if (result < 0.0) result = 0.0;
        if (result > bs_price && barrier_type == BarrierOption::BarrierType::DownAndOut)
            result = bs_price;  // KO cannot exceed vanilla
        if (result > bs_price && barrier_type == BarrierOption::BarrierType::UpAndOut)
            result = bs_price;

        return result;
    }

    // --- Bachelier (normal) model ------------------------------------------
    // Assumes normal (absolute) diffusion: dS = r*S*dt + sigma_n*dW
    // C = (F - K)*N(d) + sigma_n*sqrt(T)*N'(d)
    // P = (K - F)*N(-d) + sigma_n*sqrt(T)*N'(d)
    // where d = (F - K) / (sigma_n * sqrt(T)), F = S*exp(r*T)
    static f64 bachelier_price(OptionType type, f64 spot, f64 strike, f64 t,
                               f64 r, f64 vol) noexcept {
        if (t <= 0.0) {
            if (type == OptionType::Call) return Math::max(spot - strike, 0.0);
            else                          return Math::max(strike - spot, 0.0);
        }
        f64 F  = spot * Math::exp(r * t);
        f64 vt = vol * Math::sqrt(t);
        if (vt < 1e-15) {
            f64 intrinsic = (type == OptionType::Call) ? Math::max(F - strike, 0.0)
                                                       : Math::max(strike - F, 0.0);
            return Math::exp(-r * t) * intrinsic;
        }
        f64 d  = (F - strike) / vt;
        f64 df = Math::exp(-r * t);

        if (type == OptionType::Call)
            return df * ((F - strike) * dist::normal_cdf(d) +
                          vt * dist::normal_pdf(d));
        else
            return df * ((strike - F) * dist::normal_cdf(-d) +
                          vt * dist::normal_pdf(d));
    }

    static Opt<f64> bachelier_implied_vol(OptionType type, f64 target_price,
                                          f64 spot, f64 strike, f64 t,
                                          f64 r) noexcept {
        if (target_price <= 0.0 || t <= 0.0) return {};

        auto price_fn = [&](f64 v) -> f64 {
            return bachelier_price(type, spot, strike, t, r, v);
        };
        return solvers::implied_volatility(price_fn, target_price,
                                            1e-6, 1e3, 1e-8);
    }

    // --- Black-76 (futures / forwards) -------------------------------------
    // C = e^{-rT} * (F*N(d1) - K*N(d2))
    // P = e^{-rT} * (K*N(-d2) - F*N(-d1))
    // d1 = (ln(F/K) + 0.5*v^2*T) / (v*sqrt(T)), d2 = d1 - v*sqrt(T)
    static f64 black76_price(OptionType type, f64 forward, f64 strike, f64 t,
                             f64 r, f64 vol) noexcept {
        if (t <= 0.0) {
            if (type == OptionType::Call) return Math::max(forward - strike, 0.0);
            else                          return Math::max(strike - forward, 0.0);
        }
        f64 vt  = vol * Math::sqrt(t);
        if (vt < 1e-15) {
            if (type == OptionType::Call) return Math::max(forward - strike, 0.0);
            else                          return Math::max(strike - forward, 0.0);
        }
        f64 d1  = (::log(forward / strike) + 0.5 * vol * vol * t) / vt;
        f64 d2  = d1 - vt;
        f64 df  = Math::exp(-r * t);

        if (type == OptionType::Call)
            return df * (forward * dist::normal_cdf(d1) -
                          strike * dist::normal_cdf(d2));
        else
            return df * (strike * dist::normal_cdf(-d2) -
                          forward * dist::normal_cdf(-d1));
    }

};

} // namespace spp::quant
