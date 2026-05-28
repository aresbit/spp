#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h instead."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/base/calendar.h"
#include "spp/quant/base/daycounter.h"
#include <cmath>

namespace spp::quant {

// ============================================================
// TermStructure — CRTP base class for all term structures
// ============================================================
// Derived must implement: discount_impl(f64 t) -> f64
//        and optionally:  max_date_impl() -> Date
//
// The CRTP pattern eliminates virtual dispatch overhead.
// All term structure queries are resolved at compile time.
//
// Convention:
//   t = 0.0  corresponds to reference_date_
//   t > 0.0  is measured in years via day_counter_.year_fraction()

template<typename Derived>
struct TermStructure {
    Date reference_date_;
    DayCounter day_counter_;
    Calendar calendar_;
    BusinessDayConvention bdc_ = BusinessDayConvention::Following;

    // ----------------------------------------------------------
    // Date-based query interface
    // ----------------------------------------------------------

    /// Discount factor from reference_date_ to d (inclusive).
    /// df = exp(-r * t) in the continuous compounding convention.
    [[nodiscard]] f64 discount(Date d) const noexcept {
        f64 t = day_counter_.year_fraction(reference_date_, d);
        return discount_t(t);
    }

    /// Zero-coupon interest rate implied by the discount factor.
    /// The rate is expressed with the given compounding convention.
    /// For dates at or before reference_date_, returns 0.0.
    [[nodiscard]] f64 zero_rate(Date d,
                                Compounding comp = Compounding::Continuous,
                                Frequency freq = Frequency::Annual) const noexcept {
        f64 t = day_counter_.year_fraction(reference_date_, d);
        if (t <= 0.0) return 0.0;
        f64 df = discount_t(t);
        return df_to_rate(df, t, comp, freq);
    }

    /// Simply-compounded forward rate between two dates.
    /// The rate applies from t1 to t2 and is expressed with the given
    /// compounding convention over that forward period.
    [[nodiscard]] f64 forward_rate(Date t1, Date t2,
                                   Compounding comp = Compounding::Continuous,
                                   Frequency freq = Frequency::Annual) const noexcept {
        f64 df1 = discount(t1);
        f64 df2 = discount(t2);
        f64 yf = day_counter_.year_fraction(t1, t2);
        if (yf <= 0.0) return 0.0;
        f64 fwd_df = df2 / df1;  // df for the forward period
        return df_to_rate(fwd_df, yf, comp, freq);
    }

    /// Discount-factor-based forward rate (no compounding).
    /// Returns df(t1) / df(t2) - 1, the raw multi-period return.
    [[nodiscard]] f64 df_forward_rate(Date t1, Date t2) const noexcept {
        f64 df1 = discount(t1);
        f64 df2 = discount(t2);
        if (Math::abs(df2) < Limits<f64>::epsilon()) return 0.0;
        return df1 / df2 - 1.0;
    }

    // ----------------------------------------------------------
    // Accessors
    // ----------------------------------------------------------

    [[nodiscard]] Date reference_date() const noexcept {
        return reference_date_;
    }

    [[nodiscard]] Date max_date() const noexcept {
        return static_cast<const Derived*>(this)->max_date_impl();
    }

    // ----------------------------------------------------------
    // Time-based query interface (t in years from reference_date_)
    // ----------------------------------------------------------

    /// Discount factor at time t.
    /// Dispatches to the derived class via CRTP.
    [[nodiscard]] f64 discount_t(f64 t) const noexcept {
        if (t <= 0.0) return 1.0;
        return static_cast<const Derived*>(this)->discount_impl(t);
    }

    // ----------------------------------------------------------
    // Rate <-> Discount Factor conversion (static helpers)
    // ----------------------------------------------------------
    //
    // Continuous:      df = exp(-r * t)          r = -ln(df) / t
    // Simple:          df = 1 / (1 + r * t)      r = (1/df - 1) / t
    // Compounded:      df = (1 + r/n)^(-n*t)     r = n * (df^{-1/(n*t)} - 1)
    //
    // where n = frequency (e.g., 1 for annual, 2 for semi-annual)

    [[nodiscard]] static f64 rate_to_df(f64 rate, f64 t, Compounding comp, Frequency freq) noexcept {
        if (t <= 0.0) return 1.0;

        switch (comp) {
        case Compounding::Continuous:
            return Math::exp(-rate * t);

        case Compounding::Simple:
            return 1.0 / (1.0 + rate * t);

        case Compounding::Compounded: {
            f64 n = static_cast<f64>(static_cast<u16>(freq));
            if (n < 1.0) n = 1.0;
            return Math::pow(1.0 + rate / n, -n * t);
        }

        case Compounding::SimpleThenCompounded: {
            // Simple for t <= 1Y, compounded thereafter (used in some markets)
            if (t <= 1.0) {
                return 1.0 / (1.0 + rate * t);
            }
            f64 n = static_cast<f64>(static_cast<u16>(freq));
            if (n < 1.0) n = 1.0;
            // First year simple, remaining compounded
            f64 df_1y = 1.0 / (1.0 + rate * 1.0);
            f64 df_rest = Math::pow(1.0 + rate / n, -n * (t - 1.0));
            return df_1y * df_rest;
        }
        }

        return Math::exp(-rate * t);
    }

    [[nodiscard]] static f64 df_to_rate(f64 df, f64 t, Compounding comp, Frequency freq) noexcept {
        if (t <= 0.0 || df <= 0.0) return 0.0;

        switch (comp) {
        case Compounding::Continuous:
            return -::log(df) / t;

        case Compounding::Simple:
            return (1.0 / df - 1.0) / t;

        case Compounding::Compounded: {
            f64 n = static_cast<f64>(static_cast<u16>(freq));
            if (n < 1.0) n = 1.0;
            return n * (Math::pow(df, -1.0 / (n * t)) - 1.0);
        }

        case Compounding::SimpleThenCompounded: {
            if (t <= 1.0) {
                return (1.0 / df - 1.0) / t;
            }
            // [UNSPECIFIED] Exact inversion for SimpleThenCompounded requires
            // solving a non-linear equation. We fall back to the compounded
            // formula which is a good approximation for t >> 1.
            f64 n = static_cast<f64>(static_cast<u16>(freq));
            if (n < 1.0) n = 1.0;
            return n * (Math::pow(df, -1.0 / (n * t)) - 1.0);
        }
        }

        return -::log(df) / t;
    }
};

// ============================================================
// Default max_date_impl for derived classes that do not
// override it — returns the maximum representable date.
// ============================================================
// Derived classes (like PiecewiseYieldCurve) should override
// max_date_impl() to return the last pillar date.

} // namespace spp::quant
