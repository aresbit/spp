#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h" // for Date

namespace spp::quant {

// ---- forward declarations (superseded by Handle<T> in a later phase) ------
struct YieldCurve;
struct VolSurface;
struct Quote;

// =========================================================================
// MarketData — aggregated market data snapshot keyed by pricing date.
//
// Stores pointers to term structures and scalar parameters required to
// price most derivative instruments.  Raw pointers will be replaced by
// Handle<T> (QuantLib-style observable handles) in a later phase.
// =========================================================================
struct MarketData {
    Date as_of_;

    // ---- discount / projection curves ---------------------------------
    Opt<YieldCurve*> discount_curve_;   // risk-free discounting
    Opt<YieldCurve*> projection_curve_; // floating-rate projections

    // ---- volatility surface (for options) -------------------------------
    Opt<VolSurface*> vol_surface_;

    // ---- spot / carry --------------------------------------------------
    Opt<f64> spot_price_;
    Opt<f64> dividend_yield_; // continuous dividend yield (equity)
    Opt<f64> repo_rate_;      // repo rate (bond futures / total return)

    // ---- FX -------------------------------------------------------------
    Opt<YieldCurve*> foreign_discount_curve_;
    Opt<f64> fx_spot_;

    // ---- credit ---------------------------------------------------------
    Opt<YieldCurve*> credit_curve_; // survival / hazard-rate curve
    Opt<f64> recovery_rate_;

    // ---- convenience accessors (require the relevant field to be set) ---
    // Discount factor from as_of_ to d.
    [[nodiscard]] f64 discount(Date d) const noexcept;
    // Forward rate between t1 and t2 (actual/actual continuous compounding).
    [[nodiscard]] f64 forward_rate(Date t1, Date t2) const noexcept;
    // Black-model volatility for a given expiry and strike.
    [[nodiscard]] f64 black_vol(Date expiry, f64 strike) const noexcept;
    // Underlying spot price.
    [[nodiscard]] f64 spot() const noexcept;
};

// Accessor stubs — the real implementations will forward to the actual
// YieldCurve / VolSurface methods once those types are defined.
inline f64 MarketData::discount(Date d) const noexcept {
    // Placeholder: when YieldCurve is defined, delegate to
    // discount_curve_->discount(d).
    (void)d;
    return 1.0;
}

inline f64 MarketData::forward_rate(Date t1, Date t2) const noexcept {
    // fwd = (discount(t1)/discount(t2) - 1) / dcf(t1,t2)
    // Placeholder returns 0.
    (void)t1;
    (void)t2;
    return 0.0;
}

inline f64 MarketData::black_vol(Date expiry, f64 strike) const noexcept {
    // Placeholder: vol_surface_->black_vol(expiry, strike).
    (void)expiry;
    (void)strike;
    return 0.2; // 20% placeholder
}

inline f64 MarketData::spot() const noexcept {
    return spot_price_.ok() ? *spot_price_ : 0.0;
}

// =========================================================================
// MarketDataBuilder — incremental construction via method chaining
// =========================================================================
struct MarketDataBuilder {
    MarketData md_;

    MarketDataBuilder& with_as_of(Date d) noexcept {
        md_.as_of_ = d;
        return *this;
    }
    MarketDataBuilder& with_discount_curve(YieldCurve* yc) noexcept {
        md_.discount_curve_ = yc;
        return *this;
    }
    MarketDataBuilder& with_projection_curve(YieldCurve* yc) noexcept {
        md_.projection_curve_ = yc;
        return *this;
    }
    MarketDataBuilder& with_vol_surface(VolSurface* vs) noexcept {
        md_.vol_surface_ = vs;
        return *this;
    }
    MarketDataBuilder& with_spot(f64 s) noexcept {
        md_.spot_price_ = s;
        return *this;
    }
    MarketDataBuilder& with_dividend_yield(f64 q) noexcept {
        md_.dividend_yield_ = q;
        return *this;
    }
    MarketDataBuilder& with_repo_rate(f64 r) noexcept {
        md_.repo_rate_ = r;
        return *this;
    }
    MarketDataBuilder& with_foreign_discount_curve(YieldCurve* yc) noexcept {
        md_.foreign_discount_curve_ = yc;
        return *this;
    }
    MarketDataBuilder& with_fx_spot(f64 fx) noexcept {
        md_.fx_spot_ = fx;
        return *this;
    }
    MarketDataBuilder& with_credit_curve(YieldCurve* cc) noexcept {
        md_.credit_curve_ = cc;
        return *this;
    }
    MarketDataBuilder& with_recovery_rate(f64 rr) noexcept {
        md_.recovery_rate_ = rr;
        return *this;
    }

    MarketData build() noexcept { return spp::move(md_); }
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::MarketData, "MarketData",
                 SPP_FIELD(as_of_),
                 SPP_FIELD(discount_curve_),
                 SPP_FIELD(projection_curve_),
                 SPP_FIELD(vol_surface_),
                 SPP_FIELD(spot_price_),
                 SPP_FIELD(dividend_yield_),
                 SPP_FIELD(repo_rate_),
                 SPP_FIELD(foreign_discount_curve_),
                 SPP_FIELD(fx_spot_),
                 SPP_FIELD(credit_curve_),
                 SPP_FIELD(recovery_rate_));
