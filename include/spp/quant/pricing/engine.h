#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <concepts>

#include <spp/quant/instruments/instrument.h>
#include <spp/quant/instruments/options.h>
#include <spp/quant/instruments/bonds.h>
#include <spp/quant/data/market_data.h>
#include <spp/quant/pricing/black_scholes.h>
#include <spp/quant/pricing/binomial.h>
#include <spp/quant/math/distributions.h>

namespace spp::quant {

// =========================================================================
// PricingEngine concept — compile-time verification of engine interface.
// Any callable satisfying { e.price(instrument, market_data) -> f64 }
// can serve as a pricing engine.
// =========================================================================
template<typename Engine, typename Instrument>
concept PricingEngine = requires(Engine e, const Instrument& inst,
                                  const MarketData& mkt) {
    { e.price(inst, mkt) } -> std::same_as<f64>;
};

// =========================================================================
// Pricer — compile-time polymorphic dispatch via overload resolution.
//
// Usage:
//   f64 pv = Pricer::price(my_option, market_data);
// =========================================================================
struct Pricer {
    template<typename InstrumentType>
    static f64 price(const InstrumentType& inst, const MarketData& mkt);
};

// =========================================================================
// Dispatch functions — each instrument gets the right engine.
// =========================================================================

// --- EuropeanOption via Black-Scholes ------------------------------------
inline f64 price_european_bs(const EuropeanOption& opt,
                             const MarketData& mkt) noexcept {
    return BlackScholes::price(opt, mkt);
}

// --- AmericanOption via BinomialTree -------------------------------------
inline f64 price_american_tree(const AmericanOption& opt,
                                const MarketData& mkt) noexcept {
    BinomialTree<500> tree;
    return tree.price(opt, mkt);
}

// --- BarrierOption via Black-Scholes analytic (Reiner-Rubinstein 1991) ---
inline f64 price_barrier_bs(const BarrierOption& opt,
                             const MarketData& mkt) noexcept {
    f64 S = opt.underlying_spot_;
    f64 K = opt.strike_;
    f64 H = opt.barrier_;
    f64 T = static_cast<f64>(opt.expiry_.serial_ - mkt.as_of_.serial_) / 365.0;
    if (T <= 0.0) T = 0.0;
    f64 r = mkt.zero_rate(opt.expiry_, Compounding::Continuous, Frequency::Annual);
    f64 q = mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
    f64 v = mkt.black_vol(opt.expiry_, opt.strike_);

    return BlackScholes::barrier_price(opt.barrier_type_, opt.type_,
                                        S, K, H, T, r, q, v);
}

// --- AsianOption via geometric average closed-form (Kemna & Vorst 1990) ---
// Uses the geometric-average approximation: the geometric average of
// lognormal prices is itself lognormal, with adjusted volatility and
// cost-of-carry.  The arithmetic average is approximated by the geometric
// average with a strike adjustment (not implemented here — use Monte Carlo
// for accurate arithmetic Asian pricing).
inline f64 price_asian_geo(const AsianOption& opt,
                            const MarketData& mkt) noexcept {
    f64 S = opt.underlying_spot_;
    f64 K = opt.strike_;
    f64 T = static_cast<f64>(opt.expiry_.serial_ - mkt.as_of_.serial_) / 365.0;
    if (T <= 0.0) {
        return (opt.type_ == OptionType::Call) ? Math::max(S - K, 0.0)
                                               : Math::max(K - S, 0.0);
    }
    f64 r = mkt.zero_rate(opt.expiry_, Compounding::Continuous, Frequency::Annual);
    f64 q = mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
    f64 v = mkt.black_vol(opt.expiry_, opt.strike_);

    // Kemna & Vorst (1990): geometric-average Asian option
    // sigma_adj = sigma / sqrt(3)
    // b_adj = 0.5 * (r - q - sigma^2 / 6)
    f64 sigma_adj = v / Math::sqrt(3.0);
    f64 b_adj = 0.5 * (r - q - (v * v) / 6.0);

    // Black-Scholes generalized with cost-of-carry b:
    //   C = S*exp((b-r)T)*N(d1) - K*exp(-rT)*N(d2)
    //   P = K*exp(-rT)*N(-d2) - S*exp((b-r)T)*N(-d1)
    //   d1 = (ln(S/K) + (b + sigma^2/2)*T) / (sigma*sqrt(T))
    //   d2 = d1 - sigma*sqrt(T)
    f64 vt  = sigma_adj * Math::sqrt(T);
    if (vt < 1e-15) {
        // Zero vol — use forward
        f64 F = S * Math::exp(b_adj * T);
        f64 df = Math::exp(-r * T);
        if (opt.type_ == OptionType::Call)
            return df * Math::max(F - K, 0.0);
        else
            return df * Math::max(K - F, 0.0);
    }

    f64 d1 = (Math::log(S / K) + (b_adj + 0.5 * sigma_adj * sigma_adj) * T) / vt;
    f64 d2 = d1 - vt;
    f64 df_r = Math::exp(-r * T);
    f64 df_b = Math::exp((b_adj - r) * T);

    if (opt.type_ == OptionType::Call)
        return S * df_b * dist::normal_cdf(d1) -
               K * df_r * dist::normal_cdf(d2);
    else
        return K * df_r * dist::normal_cdf(-d2) -
               S * df_b * dist::normal_cdf(-d1);
}

// --- DigitalOption via Black-Scholes -------------------------------------
inline f64 price_digital_bs(const DigitalOption& opt,
                             const MarketData& mkt) noexcept {
    f64 S = opt.underlying_spot_;
    f64 K = opt.strike_;
    f64 T = static_cast<f64>(opt.expiry_.serial_ - mkt.as_of_.serial_) / 365.0;
    f64 r = mkt.zero_rate(opt.expiry_, Compounding::Continuous, Frequency::Annual);
    f64 q = mkt.dividend_yield_.ok() ? *mkt.dividend_yield_ : 0.0;
    f64 v = mkt.black_vol(opt.expiry_, opt.strike_);

    f64 digital = BlackScholes::digital_price(opt.type_, S, K, T, r, q, v);
    return opt.cash_rebate_ * digital;
}

// --- FixedRateBond via yield curve ---------------------------------------
inline f64 price_fixed_bond(const FixedRateBond& bond,
                            const MarketData& mkt) noexcept {
    f64 face   = bond.face_value_;
    f64 rate   = bond.coupon_rate_;
    u16 freq   = static_cast<u16>(bond.coupon_frequency_);
    f64 coupon = face * rate / static_cast<f64>(freq);

    // Generate coupon dates
    Vec<Date> cpn_dates = bond.coupon_dates();

    f64 pv = 0.0;

    // Discount each coupon
    for (u64 i = 0; i < cpn_dates.length(); i++) {
        Date d = cpn_dates[i];
        if (d > mkt.as_of_) {
            f64 df = mkt.discount(d);
            pv += coupon * df;
        }
    }

    // Discount the principal at maturity
    f64 df_mat = mkt.discount(bond.maturity_);
    pv += face * df_mat;

    // Add accrued interest (buyer compensates seller)
    f64 ai = bond.accrued_interest(mkt.as_of_);
    pv += ai;

    return pv;
}

// --- ZeroCouponBond via yield curve --------------------------------------
inline f64 price_zero_bond(const ZeroCouponBond& bond,
                            const MarketData& mkt) noexcept {
    f64 df = mkt.discount(bond.maturity_);
    return bond.face_value_ * df;
}

// =========================================================================
// Pricer::price specializations — tag-dispatch to the right engine.
// =========================================================================
template<>
inline f64 Pricer::price(const EuropeanOption& inst, const MarketData& mkt) {
    return price_european_bs(inst, mkt);
}

template<>
inline f64 Pricer::price(const AmericanOption& inst, const MarketData& mkt) {
    return price_american_tree(inst, mkt);
}

template<>
inline f64 Pricer::price(const BarrierOption& inst, const MarketData& mkt) {
    return price_barrier_bs(inst, mkt);
}

template<>
inline f64 Pricer::price(const AsianOption& inst, const MarketData& mkt) {
    return price_asian_geo(inst, mkt);
}

template<>
inline f64 Pricer::price(const DigitalOption& inst, const MarketData& mkt) {
    return price_digital_bs(inst, mkt);
}

template<>
inline f64 Pricer::price(const FixedRateBond& inst, const MarketData& mkt) {
    return price_fixed_bond(inst, mkt);
}

template<>
inline f64 Pricer::price(const ZeroCouponBond& inst, const MarketData& mkt) {
    return price_zero_bond(inst, mkt);
}

// =========================================================================
// Convenience: price an AnyOption variant via visitor.
// =========================================================================
inline f64 price_any(const AnyOption& opt_var, const MarketData& mkt) noexcept {
    return opt_var.match([&](const auto& opt) -> f64 {
        return Pricer::price(opt, mkt);
    });
}

} // namespace spp::quant
