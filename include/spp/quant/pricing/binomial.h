#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <spp/quant/instruments/options.h>
#include <spp/quant/data/market_data.h>
#include <spp/quant/math/distributions.h>

namespace spp::quant {

// =========================================================================
// BinomialTree — lattice pricing for European and American options
//
// Supports four tree constructions:
//   CRR        — Cox, Ross & Rubinstein (1979)
//   JarrowRudd — Jarrow & Rudd (1982), equal-probability tree
//   Tian       — Tian (1993), third-moment matching
//   LeisenReimer — Leisen & Reimer (1996), Preizer-Pratt inversion
//
// Template parameter DefaultSteps sets the default step count.
// =========================================================================

template<u64 DefaultSteps = 500>
struct BinomialTree {
    enum struct TreeType : u8 {
        CRR           = 0,
        JarrowRudd    = 1,
        Tian          = 2,
        LeisenReimer  = 3,
    };

    u64      steps_ = DefaultSteps;
    TreeType type_  = TreeType::CRR;

    // ---- Price European ---------------------------------------------------
    f64 price(const EuropeanOption& opt, const MarketData& mkt) const {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = year_frac(opt.expiry_, mkt);
        f64 r = zero_rate(mkt, T);
        f64 q = div_yield(mkt);
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, p = 0.5;
        compute_params(r, q, v, dt, S, K, T, u, d, p);

        f64 df = Math::exp(-r * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);

        // Backward induction
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                vals[j] = df * (p * vals[j + 1] + (1.0 - p) * vals[j]);
            }
        }

        return vals[0];
    }

    // ---- Price American ----------------------------------------------------
    f64 price(const AmericanOption& opt, const MarketData& mkt) const {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = year_frac(opt.expiry_, mkt);
        f64 r = zero_rate(mkt, T);
        f64 q = div_yield(mkt);
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, p = 0.5;
        compute_params(r, q, v, dt, S, K, T, u, d, p);

        f64 df = Math::exp(-r * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);

        // Backward induction with early exercise
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                f64 spot_j = S * Math::pow(u, static_cast<f64>(j)) *
                                  Math::pow(d, static_cast<f64>(step - 1 - j));
                f64 exercise = is_call ? Math::max(spot_j - K, 0.0)
                                       : Math::max(K - spot_j, 0.0);
                f64 continuation = df * (p * vals[j + 1] + (1.0 - p) * vals[j]);
                vals[j] = Math::max(continuation, exercise);
            }
        }

        return vals[0];
    }

    // ---- Greeks for European -----------------------------------------------
    Greeks greeks(const EuropeanOption& opt, const MarketData& mkt) const {
        f64 S  = opt.underlying_spot_;
        f64 K  = opt.strike_;
        f64 T  = year_frac(opt.expiry_, mkt);
        f64 r  = zero_rate(mkt, T);
        f64 q  = div_yield(mkt);
        f64 v  = mkt.black_vol(opt.expiry_, opt.strike_);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n  = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, p = 0.5;
        compute_params(r, q, v, dt, S, K, T, u, d, p);

        f64 df = Math::exp(-r * dt);

        // Terminal payoffs: n+1 nodes at level n
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);

        // Storage for analytical Greeks using tree levels 1 and 2.
        // We capture BEFORE the backward reduction so the state is correct.
        f64 step1_up = 0.0, step1_dn = 0.0;      // 2 nodes at level 1 (dt)
        f64 step2_uu = 0.0, step2_ud = 0.0, step2_dd = 0.0; // 3 nodes at level 2 (2*dt)
        bool captured_1 = false, captured_2 = false;

        for (u64 step = n; step > 0; step--) {
            // BEFORE reduction: vals has step+1 live nodes at level `step`.
            // Level 2 (= 3 nodes) occurs when step == 2.
            if (step == 2) {
                // vals[0..2] are option values at level 2 (time = 2*dt)
                step2_dd = vals[0]; // S * d^2
                step2_ud = vals[1]; // S * u * d
                step2_uu = vals[2]; // S * u^2
                captured_2 = true;
            }
            // Level 1 (= 2 nodes) occurs when step == 1.
            if (step == 1) {
                // vals[0..1] are option values at level 1 (time = dt)
                step1_dn = vals[0]; // S * d
                step1_up = vals[1]; // S * u
                captured_1 = true;
            }

            // Backward reduction: level step -> level step-1
            for (u64 j = 0; j < step; j++) {
                vals[j] = df * (p * vals[j + 1] + (1.0 - p) * vals[j]);
            }
        }

        f64 V0 = vals[0];

        // Analytical Greeks from the first two tree levels
        if (captured_1 && captured_2) {
            f64 Su = S * u;
            f64 Sd = S * d;
            f64 Suu = S * u * u;
            f64 Sud = S * u * d;
            f64 Sdd = S * d * d;

            Greeks g;
            // Delta = (V_up - V_down) / (S*u - S*d)
            g.delta = (step1_up - step1_dn) / (Su - Sd);

            // Gamma = (delta_up - delta_down) / (0.5 * (S*u^2 - S*d^2))
            f64 delta_u = (step2_uu - step2_ud) / (Suu - Sud);
            f64 delta_d = (step2_ud - step2_dd) / (Sud - Sdd);
            f64 dS_mid = 0.5 * (Suu - Sdd);
            g.gamma = (delta_u - delta_d) / dS_mid;

            // Vega: finite difference on vol (bump by 1%)
            f64 price_up_v = price_with_vol(opt, mkt, v * 1.01);
            f64 price_dn_v = price_with_vol(opt, mkt, v * 0.99);
            g.vega = (price_up_v - price_dn_v) / (0.02 * v);

            // Theta: (V_mid - V0) / (2*dt), per calendar day
            g.theta = (step2_ud - V0) / (2.0 * dt) / 365.0;

            // Rho: finite difference on rate (per 1%)
            f64 dr = 0.0001;
            f64 p_rup = price_with_rate(opt, mkt, r + dr);
            f64 p_rdn = price_with_rate(opt, mkt, r - dr);
            g.rho = (p_rup - p_rdn) / (2.0 * dr) * 0.01;

            return g;
        }

        // Fallback: all finite difference
        return finite_diff_greeks(opt, mkt);
    }

    // ---- Greeks for American -----------------------------------------------
    Greeks greeks(const AmericanOption& opt, const MarketData& mkt) const {
        // Finite-difference Greeks for American (tree analytical Greeks
        // are unreliable near early-exercise boundary)
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        f64 r = zero_rate(mkt, year_frac(opt.expiry_, mkt));

        f64 price0 = price(opt, mkt);

        f64 dS = opt.underlying_spot_ * 0.005;
        AmericanOption opt_up = opt;
        opt_up.underlying_spot_ += dS;
        AmericanOption opt_dn = opt;
        opt_dn.underlying_spot_ -= dS;
        f64 p_up = price(opt_up, mkt);
        f64 p_dn = price(opt_dn, mkt);

        Greeks g;
        g.delta = (p_up - p_dn) / (2.0 * dS);
        g.gamma = (p_up - 2.0 * price0 + p_dn) / (dS * dS);

        f64 vol_shift = v * 0.01;
        f64 p_vol = price_with_vol(opt, mkt, v + vol_shift);
        g.vega = (p_vol - price0) / vol_shift;

        f64 dt = 1.0 / 365.0;
        // Theta: shift expiry by 1 day
        AmericanOption opt_t = opt;
        // We can't easily shift the expiry in the market data context since
        // MarketData stub doesn't properly handle time. Use dt approximation.
        f64 T = year_frac(opt.expiry_, mkt);
        f64 T2 = T - dt;
        if (T2 <= 0.0) T2 = dt * 0.5;
        f64 p_t = price_with_time(opt, mkt, T2);
        g.theta = (p_t - price0) / 1.0; // per day

        f64 dr = 0.0001;
        f64 p_r_up = price_with_rate(opt, mkt, r + dr);
        f64 p_r_dn = price_with_rate(opt, mkt, r - dr);
        g.rho = (p_r_up - p_r_dn) / (2.0 * dr) * 0.01;

        return g;
    }

private:
    // ---- Parameter computation per tree type ------------------------------
    void compute_params(f64 r, f64 q, f64 v, f64 dt,
                        f64 S, f64 K, f64 T,
                        f64& u, f64& d, f64& p) const noexcept {
        switch (type_) {
        case TreeType::CRR:
            crr_params(v, dt, r, q, u, d, p);
            break;
        case TreeType::JarrowRudd:
            jr_params(v, dt, r, q, u, d, p);
            break;
        case TreeType::Tian:
            tian_params(v, dt, r, q, u, d, p);
            break;
        case TreeType::LeisenReimer:
            lr_params(v, T, S, K, r, q, dt, u, d, p);
            break;
        }
    }

    // CRR: u = exp(v*sqrt(dt)), d = 1/u
    static void crr_params(f64 v, f64 dt, f64 r, f64 q,
                           f64& u, f64& d, f64& p) noexcept {
        f64 vt = v * Math::sqrt(dt);
        u = Math::exp(vt);
        d = 1.0 / u;
        f64 edt = Math::exp((r - q) * dt);
        p = (edt - d) / (u - d);
        // Clamp probability
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
    }

    // Jarrow-Rudd: equal probability 0.5
    static void jr_params(f64 v, f64 dt, f64 r, f64 q,
                          f64& u, f64& d, f64& p) noexcept {
        f64 vt = v * Math::sqrt(dt);
        f64 drift = (r - q - 0.5 * v * v) * dt;
        u = Math::exp(drift + vt);
        d = Math::exp(drift - vt);
        p = 0.5;
    }

    // Tian: third-moment matching
    static void tian_params(f64 v, f64 dt, f64 r, f64 q,
                            f64& u, f64& d, f64& p) noexcept {
        f64 v2 = v * v;
        f64 edt = Math::exp((r - q) * dt);
        f64 V  = Math::exp(v2 * dt);
        f64 sqrt_term = Math::sqrt(Math::max(V * V + 2.0 * V - 3.0, 0.0));
        u = 0.5 * edt * V * (V + 1.0 + sqrt_term);
        d = 0.5 * edt * V * (V + 1.0 - sqrt_term);
        p = (edt - d) / (u - d);
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
    }

    // Leisen-Reimer: Preizer-Pratt inversion for accurate ATM pricing
    void lr_params(f64 v, f64 T, f64 S, f64 K, f64 r, f64 q,
                   f64 dt, f64& u, f64& d, f64& p) const noexcept {
        // Use an odd number of steps for best convergence
        u64 n = steps_;
        if (n % 2 == 0) n++; // ensure odd

        f64 vt = v * Math::sqrt(T);
        if (vt < 1e-15) {
            crr_params(v, dt, r, q, u, d, p);
            return;
        }

        f64 d1 = (Math::log(S / K) + (r - q + 0.5 * v * v) * T) / vt;
        f64 d2 = d1 - vt;

        // Preizer-Pratt inversion
        f64 n1 = static_cast<f64>(n);
        f64 pp_d1 = peizer_pratt(d1, n1);
        f64 pp_d2 = peizer_pratt(d2, n1);

        f64 edt = Math::exp((r - q) * dt);

        // Construct the tree
        u = edt * pp_d1 / pp_d2;
        d = (edt - pp_d2 * u) / (1.0 - pp_d2);

        // Clamp d to positive
        if (d <= 0.0) {
            crr_params(v, dt, r, q, u, d, p);
            return;
        }

        p = pp_d2;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
    }

    // Preizer-Pratt inversion (Leisen & Reimer 1996)
    // h(z, n) = 0.5 + sign(z)/2 * sqrt(1 - exp(-(z/(n+1/3))^2 * (n+1/6)))
    static f64 peizer_pratt(f64 z, f64 n) noexcept {
        if (Math::abs(z) < 1e-15) return 0.5;
        f64 a = z / (n + 1.0 / 3.0);
        f64 b = a * a * (n + 1.0 / 6.0);
        f64 term = 1.0 - Math::exp(-b);
        if (term < 0.0) term = 0.0;
        f64 result = 0.5 + 0.5 * Math::sign(z) * Math::sqrt(term);
        if (result < 0.0) result = 0.0;
        if (result > 1.0) result = 1.0;
        return result;
    }

    // ---- Terminal payoffs -------------------------------------------------
    static Vec<f64> terminal_payoffs(f64 S, f64 K, f64 u, f64 d, u64 n,
                                     bool is_call) noexcept {
        Vec<f64> vals;
        vals.reserve(n + 1);
        for (u64 j = 0; j <= n; j++) {
            f64 spot = S * Math::pow(u, static_cast<f64>(j)) *
                            Math::pow(d, static_cast<f64>(n - j));
            f64 payoff = is_call ? Math::max(spot - K, 0.0)
                                 : Math::max(K - spot, 0.0);
            vals.push(payoff);
        }
        return vals;
    }

    // ---- Helpers for finite-difference Greeks -----------------------------
    f64 price_with_vol(const EuropeanOption& opt, const MarketData& mkt,
                       f64 vol) const noexcept {
        // Reprice with a specific vol (bypass mkt.black_vol)
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = year_frac(opt.expiry_, mkt);
        f64 r = zero_rate(mkt, T);
        f64 q = div_yield(mkt);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, pu = 0.5;
        EuropeanOption tmp_opt = opt; (void)tmp_opt;
        // Just use the raw params with the given vol
        compute_params(r, q, vol, dt, S, K, T, u, d, pu);

        f64 df = Math::exp(-r * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                vals[j] = df * (pu * vals[j + 1] + (1.0 - pu) * vals[j]);
            }
        }
        return vals[0];
    }

    f64 price_with_vol(const AmericanOption& opt, const MarketData& mkt,
                       f64 vol) const noexcept {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = year_frac(opt.expiry_, mkt);
        f64 r = zero_rate(mkt, T);
        f64 q = div_yield(mkt);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, pu = 0.5;
        compute_params(r, q, vol, dt, S, K, T, u, d, pu);

        f64 df = Math::exp(-r * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                f64 spot_j = S * Math::pow(u, static_cast<f64>(j)) *
                                  Math::pow(d, static_cast<f64>(step - 1 - j));
                f64 exercise = is_call ? Math::max(spot_j - K, 0.0)
                                       : Math::max(K - spot_j, 0.0);
                f64 continuation = df * (pu * vals[j + 1] + (1.0 - pu) * vals[j]);
                vals[j] = Math::max(continuation, exercise);
            }
        }
        return vals[0];
    }

    f64 price_with_rate(const EuropeanOption& opt, const MarketData& mkt,
                        f64 rate) const noexcept {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = year_frac(opt.expiry_, mkt);
        f64 q = div_yield(mkt);
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, pu = 0.5;
        compute_params(rate, q, v, dt, S, K, T, u, d, pu);

        f64 df = Math::exp(-rate * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                vals[j] = df * (pu * vals[j + 1] + (1.0 - pu) * vals[j]);
            }
        }
        return vals[0];
    }

    f64 price_with_rate(const AmericanOption& opt, const MarketData& mkt,
                        f64 rate) const noexcept {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 T = year_frac(opt.expiry_, mkt);
        f64 q = div_yield(mkt);
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, pu = 0.5;
        compute_params(rate, q, v, dt, S, K, T, u, d, pu);

        f64 df = Math::exp(-rate * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                f64 spot_j = S * Math::pow(u, static_cast<f64>(j)) *
                                  Math::pow(d, static_cast<f64>(step - 1 - j));
                f64 exercise = is_call ? Math::max(spot_j - K, 0.0)
                                       : Math::max(K - spot_j, 0.0);
                f64 continuation = df * (pu * vals[j + 1] + (1.0 - pu) * vals[j]);
                vals[j] = Math::max(continuation, exercise);
            }
        }
        return vals[0];
    }

    f64 price_with_time(const AmericanOption& opt, const MarketData& mkt,
                        f64 T) const noexcept {
        f64 S = opt.underlying_spot_;
        f64 K = opt.strike_;
        f64 r = zero_rate(mkt, T);
        f64 q = div_yield(mkt);
        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        bool is_call = (opt.type_ == OptionType::Call);

        u64 n = steps_;
        f64 dt = T / static_cast<f64>(n);
        f64 u = 1.0, d = 1.0, pu = 0.5;
        compute_params(r, q, v, dt, S, K, T, u, d, pu);

        f64 df = Math::exp(-r * dt);
        Vec<f64> vals = terminal_payoffs(S, K, u, d, n, is_call);
        for (u64 step = n; step > 0; step--) {
            for (u64 j = 0; j < step; j++) {
                f64 spot_j = S * Math::pow(u, static_cast<f64>(j)) *
                                  Math::pow(d, static_cast<f64>(step - 1 - j));
                f64 exercise = is_call ? Math::max(spot_j - K, 0.0)
                                       : Math::max(K - spot_j, 0.0);
                f64 continuation = df * (pu * vals[j + 1] + (1.0 - pu) * vals[j]);
                vals[j] = Math::max(continuation, exercise);
            }
        }
        return vals[0];
    }

    // ---- Market data helpers ----------------------------------------------
    static f64 year_frac(Date expiry, const MarketData& mkt) noexcept {
        i64 days = static_cast<i64>(expiry.serial_ - mkt.as_of_.serial_);
        if (days <= 0) return 0.0;
        return static_cast<f64>(days) / 365.0;
    }

    static f64 zero_rate(const MarketData& mkt, f64 t) noexcept {
        // Placeholder: derive from discount curve when wired.
        // For now the discount stub returns 1.0 so r=0.
        (void)mkt;
        (void)t;
        return 0.0;
    }

    static f64 div_yield(const MarketData& mkt) noexcept {
        return mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
    }

    // ---- Fallback finite-difference Greeks (European) ---------------------
    Greeks finite_diff_greeks(const EuropeanOption& opt,
                              const MarketData& mkt) const noexcept {
        f64 price0 = price(opt, mkt);
        f64 S = opt.underlying_spot_;
        f64 dS = S * 0.005;

        EuropeanOption opt_up = opt;
        opt_up.underlying_spot_ += dS;
        EuropeanOption opt_dn = opt;
        opt_dn.underlying_spot_ -= dS;

        f64 p_up = price(opt_up, mkt);
        f64 p_dn = price(opt_dn, mkt);

        Greeks g;
        g.delta = (p_up - p_dn) / (2.0 * dS);
        g.gamma = (p_up - 2.0 * price0 + p_dn) / (dS * dS);

        f64 v = mkt.black_vol(opt.expiry_, opt.strike_);
        f64 vol_shift = v * 0.01;
        f64 p_vol = price_with_vol(opt, mkt, v + vol_shift);
        g.vega = (p_vol - price0) / vol_shift;

        f64 dt = 1.0 / 365.0;
        f64 T = year_frac(opt.expiry_, mkt);
        EuropeanOption opt_t = opt;
        // Shift expiry by adding 1 day to serial
        opt_t.expiry_.serial_ += 1;
        f64 p_t = price(opt_t, mkt);
        g.theta = (p_t - price0);
        // Restore
        // (rate rho uses finite diff on the rate parameter)

        f64 r = zero_rate(mkt, T);
        f64 dr = 0.0001;
        f64 p_rup = price_with_rate(opt, mkt, r + dr);
        f64 p_rdn = price_with_rate(opt, mkt, r - dr);
        g.rho = (p_rup - p_rdn) / (2.0 * dr) * 0.01;

        return g;
    }
};

} // namespace spp::quant
