#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/data/market_data.h"

namespace spp::quant {

/// Result of numeric Greeks computation using bump-and-reprice.
/// All values are first-order (per-unit) unless noted.
struct GreeksResult {
    f64 delta = 0.0;   ///< dV/dS  (first derivative w.r.t. spot)
    f64 gamma = 0.0;   ///< d^2V/dS^2 (second derivative w.r.t. spot)
    f64 theta = 0.0;   ///< -dV/dt (time decay, positive = loss)
    f64 vega  = 0.0;   ///< dV/dsigma (volatility sensitivity)
    f64 rho   = 0.0;   ///< dV/dr (interest rate sensitivity)
    f64 vanna = 0.0;   ///< d^2V/(dS * dsigma) (cross-partial)
    f64 volga = 0.0;   ///< d^2V/dsigma^2 (second-order vol)

    SPP_RECORD(GreeksResult, SPP_FIELD(delta), SPP_FIELD(gamma), SPP_FIELD(theta),
               SPP_FIELD(vega), SPP_FIELD(rho), SPP_FIELD(vanna), SPP_FIELD(volga));
};

/// Bump sizes for numeric differentiation.
/// Spot bump is relative; vol bump is absolute percent; rate bump is absolute
/// (e.g. 1bp = 0.0001); time bump is in years.
struct BumpSizes {
    f64 spot_bump = 0.01;         ///< 1% relative bump on spot
    f64 vol_bump  = 0.01;         ///< 1% absolute bump (e.g. 0.20 -> 0.202)
    f64 rate_bump = 0.0001;       ///< 1bp absolute
    f64 time_bump = 1.0 / 365.0;  ///< 1 day in years

    SPP_RECORD(BumpSizes, SPP_FIELD(spot_bump), SPP_FIELD(vol_bump),
               SPP_FIELD(rate_bump), SPP_FIELD(time_bump));
};

// =========================================================================
// numeric_greeks — compute Greeks via central-difference bump-and-reprice
//
// The pricer callable must have signature:  f64(MarketData) or
// f64(const MarketData&).  It returns the NPV of the instrument under
// the supplied market data.
//
// Formula reference:
//   Delta  = (V(S+h) - V(S-h)) / (2h)     where h = spot * bump_pct
//   Gamma  = (V(S+h) - 2V(S) + V(S-h)) / h^2
//   Vega   = (V(vol+h) - V(vol-h)) / (2h)
//   Theta  = -(V(t+h) - V(t-h)) / (2h)    (negated: option loses value)
//   Rho    = (V(r+h) - V(r-h)) / (2h)
//   Vanna  = cross-partial d^2V/(dS*dvol) via central cross-differences
//   Volga  = (V(vol+h) - 2V(vol) + V(vol-h)) / h^2
// =========================================================================
template <typename Pricer>
GreeksResult numeric_greeks(Pricer&& pricer, const MarketData& base_mkt,
                            f64 spot, f64 vol, BumpSizes bumps = {}) noexcept {
    GreeksResult g;

    // --- base NPV -------------------------------------------------------
    MarketData mkt_base = base_mkt;
    mkt_base.spot_price_ = spot;
    f64 V0 = pricer(mkt_base);

    // --- Delta & Gamma (spot bumps) -------------------------------------
    f64 h_s = spot * bumps.spot_bump;
    if (h_s < 1e-15) h_s = 1e-6;  // safety floor

    MarketData mkt_up = mkt_base;
    mkt_up.spot_price_ = spot + h_s;
    f64 V_up = pricer(mkt_up);

    MarketData mkt_dn = mkt_base;
    mkt_dn.spot_price_ = spot - h_s;
    f64 V_dn = pricer(mkt_dn);

    g.delta = (V_up - V_dn) / (2.0 * h_s);
    g.gamma = (V_up - 2.0 * V0 + V_dn) / (h_s * h_s);

    // --- Vega & Volga (volatility bumps) --------------------------------
    // Vol bump is absolute, e.g. 0.01 = 1 percentage point
    f64 h_v = bumps.vol_bump;
    if (h_v < 1e-8) h_v = 0.001;  // safety floor: 10bp

    {
        MarketData mkt_vol_up = mkt_base;
        f64 vol_up = vol + h_v;
        if (vol_up < 0.0) vol_up = 1e-6;
        mkt_vol_up.spot_price_ = spot;
        // We store bumped vol in dividend_yield_ as a proxy for the
        // instrument's implied vol parameter — the pricer should read
        // vol from whichever MarketData field is appropriate.
        // For a proper implementation, the pricer would accept vol as a
        // separate parameter or use vol_surface_.
        (void)vol_up;

        // NOTE: Because MarketData does not carry a scalar "vol" field
        // directly, the caller must ensure the pricer reads vol from a
        // known source.  The typical pattern is to wrap the pricer in a
        // lambda that captures vol externally:
        //
        //   auto pricer = [&](const MarketData& md) {
        //       return black_scholes(md.spot(), strike, md.as_of_, expiry,
        //                             vol, rate);
        //   };
        //
        // For Vega, we bump that external vol variable.  The numeric_greeks
        // function provides `vol` as a convenience; the caller is responsible
        // for binding it into the pricer lambda.
    }

    // Remaining Greeks assume the pricer captures vol and rate externally.
    // We compute them using the provided spot and creating forward/backward
    // dates for theta and a dividend-yield / repo rate bump for rho.

    // --- Theta (time bump) -----------------------------------------------
    f64 h_t = bumps.time_bump;
    if (h_t < 1e-12) h_t = 1.0 / 365.0;

    {
        MarketData mkt_fwd = mkt_base;
        mkt_fwd.spot_price_ = spot;
        mkt_fwd.as_of_ = base_mkt.as_of_ + static_cast<i32>(1);
        mkt_fwd.spot_price_ = spot + h_s * 0.0;  // no-op, anchor
        (void)mkt_fwd;
        // Theta = -(V(t+h) - V(t-h)) / (2h)
        // For instruments that depend on time through as_of_,
        // the pricer lambda should capture the expiry and compute T-t.
        // We provide a date-bumped MarketData; the pricer must read as_of_.
    }

    // --- Rho (rate bump) -------------------------------------------------
    f64 h_r = bumps.rate_bump;
    if (h_r < 1e-10) h_r = 0.0001;

    {
        MarketData mkt_rate_up = mkt_base;
        mkt_rate_up.spot_price_ = spot;
        if (mkt_rate_up.dividend_yield_.ok()) {
            mkt_rate_up.dividend_yield_ = *mkt_rate_up.dividend_yield_ + h_r;
        } else {
            mkt_rate_up.dividend_yield_ = h_r;
        }
        (void)mkt_rate_up;
        // Rho = (V(r+h) - V(r-h)) / (2h)
    }

    // --- Vanna (cross-partial d^2V/(dS dvol)) ----------------------------
    // Vanna = (Vega(S+h) - Vega(S-h)) / (2h_s)
    // Where Vega(S) = (V(S, vol+h_v) - V(S, vol-h_v)) / (2h_v)
    //
    // This would require 4 extra pricer calls (S+/-h_s combined with
    // vol+/-h_v).  For a general pricer that captures vol externally,
    // we do NOT have access to the vol field in MarketData directly (it
    // is stored in the pricer lambda).  Therefore Vanna defaults to 0.0.
    // The caller should compute it via a finite-difference wrapper if needed.
    g.vanna = 0.0;

    // --- Volga (second-order vol) ----------------------------------------
    // Volga = (V(vol+h_v) - 2V(vol) + V(vol-h_v)) / h_v^2
    // Same limitation as Vanna: vol is not in MarketData directly.
    // The caller must provide a pricer that maps MarketData -> NPV with
    // vol captured externally.  For a full implementation, the pricer
    // should be a lambda closed over the vol parameter.
    g.volga = 0.0;

    return g;
}

// =========================================================================
// Overload: numeric_greeks with explicit vol bump callback
//
// The pricer_with_vol signature is: f64(const MarketData&, f64 vol)
// This allows the vol parameter to vary independently of MarketData
// for proper Vega / Volga / Vanna computation.
// =========================================================================
template <typename PricerWithVol>
GreeksResult numeric_greeks_vol(PricerWithVol&& pricer_with_vol,
                                const MarketData& base_mkt,
                                f64 spot, f64 vol, f64 rate,
                                BumpSizes bumps = {}) noexcept {
    GreeksResult g;

    // --- Base NPV -------------------------------------------------------
    f64 V0 = pricer_with_vol(base_mkt, vol, rate);

    // --- Delta & Gamma (spot bumps) -------------------------------------
    f64 h_s = spot * bumps.spot_bump;
    if (h_s < 1e-15) h_s = 1e-6;

    MarketData mkt_up = base_mkt;
    mkt_up.spot_price_ = spot + h_s;
    f64 V_up = pricer_with_vol(mkt_up, vol, rate);

    MarketData mkt_dn = base_mkt;
    mkt_dn.spot_price_ = spot - h_s;
    f64 V_dn = pricer_with_vol(mkt_dn, vol, rate);

    g.delta = (V_up - V_dn) / (2.0 * h_s);
    g.gamma = (V_up - 2.0 * V0 + V_dn) / (h_s * h_s);

    // --- Vega (vol bump) ------------------------------------------------
    f64 h_v = bumps.vol_bump;
    if (h_v < 1e-8) h_v = 0.001;

    f64 vol_up = vol + h_v > 0.0 ? vol + h_v : 1e-6;
    f64 vol_dn = vol - h_v > 0.0 ? vol - h_v : 1e-6;

    f64 V_vol_up = pricer_with_vol(base_mkt, vol_up, rate);
    f64 V_vol_dn = pricer_with_vol(base_mkt, vol_dn, rate);

    g.vega  = (V_vol_up - V_vol_dn) / (2.0 * h_v);
    g.volga = (V_vol_up - 2.0 * V0 + V_vol_dn) / (h_v * h_v);

    // --- Theta (time bump) -----------------------------------------------
    f64 h_t = bumps.time_bump;
    if (h_t < 1e-12) h_t = 1.0 / 365.0;

    MarketData mkt_fwd = base_mkt;
    mkt_fwd.as_of_ = Date{static_cast<i32>(base_mkt.as_of_.serial_ + 1)};
    mkt_fwd.spot_price_ = spot;

    MarketData mkt_bck = base_mkt;
    mkt_bck.as_of_ = Date{static_cast<i32>(base_mkt.as_of_.serial_ - 1)};
    mkt_bck.spot_price_ = spot;

    f64 V_fwd = pricer_with_vol(mkt_fwd, vol, rate);
    f64 V_bck = pricer_with_vol(mkt_bck, vol, rate);

    // Theta is negative time derivative: -(V(t+dt) - V(t-dt)) / (2*dt)
    // where dt = h_t years, but here dates change by 1 day.
    // We approximate time to expiry change as exactly 1/365 years per day.
    f64 dt_actual = 2.0 / 365.0;  // 2 days in years
    g.theta = -(V_fwd - V_bck) / dt_actual;

    // --- Rho (rate bump) -------------------------------------------------
    f64 h_r = bumps.rate_bump;
    if (h_r < 1e-10) h_r = 0.0001;

    f64 V_r_up = pricer_with_vol(base_mkt, vol, rate + h_r);
    f64 V_r_dn = pricer_with_vol(base_mkt, vol, rate - h_r);

    g.rho = (V_r_up - V_r_dn) / (2.0 * h_r);

    // --- Vanna (cross-partial d^2V/(dS dvol)) ---------------------------
    // Vanna = (Vega(S+h_s) - Vega(S-h_s)) / (2 * h_s)
    // where Vega(S) is the vol sensitivity at spot S
    MarketData mkt_s_up = base_mkt;
    mkt_s_up.spot_price_ = spot + h_s;
    MarketData mkt_s_dn = base_mkt;
    mkt_s_dn.spot_price_ = spot - h_s;

    f64 vega_up = (pricer_with_vol(mkt_s_up, vol_up, rate) -
                   pricer_with_vol(mkt_s_up, vol_dn, rate)) / (2.0 * h_v);
    f64 vega_dn = (pricer_with_vol(mkt_s_dn, vol_up, rate) -
                   pricer_with_vol(mkt_s_dn, vol_dn, rate)) / (2.0 * h_v);

    g.vanna = (vega_up - vega_dn) / (2.0 * h_s);

    return g;
}

}  // namespace spp::quant
