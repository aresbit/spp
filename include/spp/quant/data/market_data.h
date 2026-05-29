#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/termstructures/yield_curve.h"
#include "spp/quant/termstructures/vol_surface.h"

namespace spp::quant {

// ---- concrete type aliases (superseded by Handle<T> in a later phase) ------
using YieldCurve = PiecewiseYieldCurve<interp::Interpolation<>>;
using VolSurface = BlackVolSurface<>;
struct Quote;

// =========================================================================
// MarketData — aggregated market data snapshot keyed by pricing date.
//
// Stores pointers to term structures and scalar parameters required to
// price most derivative instruments.  Raw pointers will be replaced by
// Handle<T> (QuantLib-style observable handles) in a later phase.
//
// Accessor methods delegate to the stored term structures when set,
// falling back to sensible defaults when curves/surfaces are absent.
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

    // ---- convenience accessors ------------------------------------------

    /// Discount factor from as_of_ to d.
    /// Delegates to discount_curve_->discount(d) when a curve is set;
    /// falls back to 1.0 (flat zero-rate curve).
    [[nodiscard]] f64 discount(Date d) const noexcept;

    /// Simply-compounded forward rate between t1 and t2.
    /// Delegates to discount_curve_->forward_rate() when available.
    [[nodiscard]] f64 forward_rate(Date t1, Date t2) const noexcept;

    /// Zero-coupon rate implied by the discount factor between as_of_ and d.
    /// Delegates to discount_curve_->zero_rate() when a curve is set;
    /// falls back to 0.0 (flat curve).
    [[nodiscard]] f64 zero_rate(Date d,
                                 Compounding comp = Compounding::Continuous,
                                 Frequency freq = Frequency::Annual) const noexcept;

    /// Continuously-compounded zero rate — convenience shorthand for
    /// zero_rate(d, Compounding::Continuous, Frequency::Annual).
    [[nodiscard]] f64 continuous_rate(Date d) const noexcept;

    /// Black-model implied volatility for a given expiry and strike.
    /// Delegates to vol_surface_->black_vol() when a surface is set;
    /// falls back to 0.2 (20% flat vol).
    [[nodiscard]] f64 black_vol(Date expiry, f64 strike) const noexcept;

    /// Underlying spot price.
    [[nodiscard]] f64 spot() const noexcept;
};

// =========================================================================
// Accessor implementations
// =========================================================================

inline f64 MarketData::discount(Date d) const noexcept {
    if (discount_curve_.ok()) {
        YieldCurve* yc = *discount_curve_;
        if (yc != null) {
            return yc->discount(d);
        }
    }
    // Fallback: flat curve, df = 1.0 everywhere
    if (d <= as_of_) return 1.0;
    return 1.0;
}

inline f64 MarketData::forward_rate(Date t1, Date t2) const noexcept {
    if (discount_curve_.ok()) {
        YieldCurve* yc = *discount_curve_;
        if (yc != null) {
            return yc->forward_rate(t1, t2,
                                     Compounding::Continuous,
                                     Frequency::Annual);
        }
    }
    // Fallback: compute from discount factors with simple day-count
    f64 df1 = discount(t1);
    f64 df2 = discount(t2);
    i64 days = static_cast<i64>(t2.serial_ - t1.serial_);
    if (days <= 0 || df2 <= 0.0) return 0.0;
    f64 yf = static_cast<f64>(days) / 365.0;
    return (df1 / df2 - 1.0) / yf;
}

inline f64 MarketData::zero_rate(Date d,
                                   Compounding comp,
                                   Frequency freq) const noexcept {
    if (discount_curve_.ok()) {
        YieldCurve* yc = *discount_curve_;
        if (yc != null) {
            return yc->zero_rate(d, comp, freq);
        }
    }
    // Fallback: flat 0% curve
    return 0.0;
}

inline f64 MarketData::continuous_rate(Date d) const noexcept {
    return zero_rate(d, Compounding::Continuous, Frequency::Annual);
}

inline f64 MarketData::black_vol(Date expiry, f64 strike) const noexcept {
    if (vol_surface_.ok()) {
        VolSurface* vs = *vol_surface_;
        if (vs != null) {
            return vs->black_vol(expiry, strike);
        }
    }
    // Fallback: flat 20% volatility
    return 0.2;
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
        md_.discount_curve_.emplace(yc);
        return *this;
    }
    MarketDataBuilder& with_projection_curve(YieldCurve* yc) noexcept {
        md_.projection_curve_.emplace(yc);
        return *this;
    }
    MarketDataBuilder& with_vol_surface(VolSurface* vs) noexcept {
        md_.vol_surface_.emplace(vs);
        return *this;
    }
    MarketDataBuilder& with_spot(f64 s) noexcept {
        md_.spot_price_.emplace(s);
        return *this;
    }
    MarketDataBuilder& with_dividend_yield(f64 q) noexcept {
        md_.dividend_yield_.emplace(q);
        return *this;
    }
    MarketDataBuilder& with_repo_rate(f64 r) noexcept {
        md_.repo_rate_.emplace(r);
        return *this;
    }
    MarketDataBuilder& with_foreign_discount_curve(YieldCurve* yc) noexcept {
        md_.foreign_discount_curve_.emplace(yc);
        return *this;
    }
    MarketDataBuilder& with_fx_spot(f64 fx) noexcept {
        md_.fx_spot_.emplace(fx);
        return *this;
    }
    MarketDataBuilder& with_credit_curve(YieldCurve* cc) noexcept {
        md_.credit_curve_.emplace(cc);
        return *this;
    }
    MarketDataBuilder& with_recovery_rate(f64 rr) noexcept {
        md_.recovery_rate_.emplace(rr);
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
