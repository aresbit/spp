#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h instead."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/base/calendar.h"
#include "spp/quant/base/daycounter.h"
#include "spp/quant/termstructures/termstructure.h"
#include "spp/quant/math/interpolation.h"
#include "spp/core/variant.h"
#include <cmath>

namespace spp::quant {

// ============================================================
// Rate Helpers — concrete instrument types for bootstrapping
// ============================================================
//
// Each helper type encapsulates the formula to compute the implied
// discount factor at its maturity, given a way to look up discount
// factors at earlier dates (the df_lookup callable).
//
// No virtual functions. Dispatch is done via spp::Variant.

// ----------------------------------------------------------
// DepositRateHelper — cash deposit (LIBOR-style fixing)
// ----------------------------------------------------------
// Quote is the simple annualized deposit rate.
//   df(maturity) = 1 / (1 + quote * t)
struct DepositRateHelper {
    Date maturity_;
    f64 quote_;

    /// Compute the implied discount factor at this helper's maturity.
    /// df_lookup: (Date) -> f64  — returns df at dates on or before maturity
    ///                            from already-bootstrapped pillars.
    template<typename F>
    [[nodiscard]] f64 implied_df(Date ref_date, DayCounter dc, F&& /*df_lookup*/) const noexcept {
        f64 t = dc.year_fraction(ref_date, maturity_);
        if (t <= 0.0) return 1.0;
        return 1.0 / (1.0 + quote_ * t);
    }
};

// ----------------------------------------------------------
// FraRateHelper — Forward Rate Agreement
// ----------------------------------------------------------
// Quote is the simply-compounded forward rate.
// The FRA covers the period [maturity - fra_period_, maturity].
//
//   FRA rate r = (df(start) / df(end) - 1) / tau
//   => df(end) = df(start) / (1 + r * tau)
struct FraRateHelper {
    Date maturity_;
    f64 quote_;
    Period fra_period_;       // length of the FRA period (e.g., 6M for 6x12)

    template<typename F>
    [[nodiscard]] f64 implied_df(Date ref_date, DayCounter dc, F&& df_lookup) const noexcept {
        Date fixing_date = maturity_ - fra_period_;
        f64 tau = dc.year_fraction(fixing_date, maturity_);
        if (tau <= 0.0) return df_lookup(fixing_date);

        f64 df_start = df_lookup(fixing_date);  // from earlier bootstrapped pillars
        return df_start / (1.0 + quote_ * tau);
    }
};

// ----------------------------------------------------------
// SwapRateHelper — par interest-rate swap
// ----------------------------------------------------------
// Quote is the par swap rate (fixed rate that makes PV = 0).
//
// Fixed leg:  rate * sum_i( tau_i * df(T_i) )
// Float leg:  df(T_0) - df(T_n) = 1 - df(maturity)  (single-curve, unit notional)
//
// Par condition:
//   rate * [sum_{i<n} tau_i * df(T_i) + tau_n * df(T_n)] = 1 - df(T_n)
//   => df(T_n) = (1 - rate * sum_{i<n} tau_i * df(T_i)) / (1 + rate * tau_n)
//
// accrual_dates_ and accrual_periods_ define the fixed-leg payment schedule.
// The last element of both vectors corresponds to the final period ending
// at maturity.
struct SwapRateHelper {
    Date maturity_;
    f64 quote_;
    Vec<Date> accrual_dates_;    // fixed-leg payment dates (last = maturity)
    Vec<f64> accrual_periods_;   // year fractions for each accrual period

    template<typename F>
    [[nodiscard]] f64 implied_df(Date ref_date, DayCounter dc, F&& df_lookup) const noexcept {
        u64 n = accrual_dates_.length();
        if (n == 0) return 1.0;

        // Sum fixed-leg PV for all but the last payment date
        f64 pv_fixed = 0.0;
        for (u64 i = 0; i < n - 1; i++) {
            f64 df_i = df_lookup(accrual_dates_[i]);
            pv_fixed += accrual_periods_[i] * df_i;
        }

        f64 tau_last = accrual_periods_[n - 1];
        f64 df_maturity = (1.0 - quote_ * pv_fixed) / (1.0 + quote_ * tau_last);

        // Clamp to valid range in case of numerical noise
        if (df_maturity <= 0.0) df_maturity = Limits<f64>::epsilon();
        if (df_maturity > 1.0) df_maturity = 1.0;

        return df_maturity;
    }
};

// ----------------------------------------------------------
// Unified RateHelper type via Variant dispatch
// ----------------------------------------------------------
using RateHelper = Variant<DepositRateHelper, FraRateHelper, SwapRateHelper>;

/// Extract maturity date from any RateHelper variant.
[[nodiscard]] inline Date rate_helper_maturity(const RateHelper& h) noexcept {
    Date result;
    h.match(Overload{
        [&](const DepositRateHelper& r) { result = r.maturity_; },
        [&](const FraRateHelper& r)      { result = r.maturity_; },
        [&](const SwapRateHelper& r)     { result = r.maturity_; },
    });
    return result;
}

/// Extract quote from any RateHelper variant.
[[nodiscard]] inline f64 rate_helper_quote(const RateHelper& h) noexcept {
    f64 result = 0.0;
    h.match(Overload{
        [&](const DepositRateHelper& r) { result = r.quote_; },
        [&](const FraRateHelper& r)      { result = r.quote_; },
        [&](const SwapRateHelper& r)     { result = r.quote_; },
    });
    return result;
}

// ============================================================
// PiecewiseYieldCurve — bootstrapped yield curve
// ============================================================
//
// Constructed by sequential bootstrapping of market instruments
// (deposits, FRAs, swaps) from shortest to longest maturity.
//
// Template parameter InterpType controls how log-discount-factors
// are interpolated between pillar dates.
//
// During bootstrap, discount factors at intermediate dates are
// obtained by linear interpolation of log(df) over already-built pillars.
// The final curve uses the specified InterpType for query-time interpolation.

template<typename InterpType = interp::Interpolation<>>
struct PiecewiseYieldCurve : TermStructure<PiecewiseYieldCurve<InterpType>> {
    using Base = TermStructure<PiecewiseYieldCurve<InterpType>>;

    Vec<Date> pillar_dates_;       // bootstrap pillar dates (sorted ascending)
    Vec<f64> discount_factors_;    // df at each pillar date
    InterpType log_df_interp_;     // interpolation over (time, -log(df))

    // ----------------------------------------------------------
    // Bootstrap from rate helpers
    // ----------------------------------------------------------
    //
    // Algorithm:
    //   1. Sort helpers by maturity date.
    //   2. For each helper in maturity order:
    //      a. Build a df_lookup function that linearly interpolates log(df)
    //         over already-bootstrapped pillar dates.
    //      b. Use the helper's formula to compute the discount factor
    //         at its maturity (may depend on df_lookup for earlier dates).
    //      c. If the maturity date is already a pillar (overlapping
    //         instrument), skip it — the existing df takes precedence.
    //      d. Otherwise, add (maturity, df) as a new pillar.
    //   3. Build the final interpolation over (t, -log(df)).
    //
    // Robustness guarantees:
    //   - Overlapping maturities: first instrument wins, duplicate skipped.
    //   - Irregular maturities: each instrument bootstraps independently.
    //   - Out-of-order helpers: sorted internally before bootstrapping.
    //   - Numerical safety: discount factors clamped to (0, 1].

    [[nodiscard]] static PiecewiseYieldCurve bootstrap(
        Date reference_date,
        Vec<RateHelper> helpers,
        DayCounter dc = DayCounter::actual_365_fixed(),
        Calendar cal = Calendar::weekend_only()) noexcept {

        PiecewiseYieldCurve curve;
        curve.reference_date_ = reference_date;
        curve.day_counter_ = dc;
        curve.calendar_ = cal;

        u64 n_helpers = helpers.length();
        if (n_helpers == 0) {
            // Empty curve: discount factor is 1.0 everywhere.
            // discount_impl() checks pillar count and returns 1.0 directly.
            curve.pillar_dates_ = Vec<Date>{};
            curve.discount_factors_ = Vec<f64>{};
            curve.log_df_interp_ = InterpType::linear(Vec<f64>{}, Vec<f64>{});
            return curve;
        }

        // -- Step 1: Sort helpers by maturity via index array --
        Vec<u64> order = Vec<u64>::make(n_helpers);
        for (u64 i = 0; i < n_helpers; i++) {
            order[i] = i;
        }

        // Insertion sort by maturity date (ascending)
        for (u64 i = 1; i < n_helpers; i++) {
            u64 key = order[i];
            Date key_date = rate_helper_maturity(helpers[key]);
            i64 j = static_cast<i64>(i) - 1;
            while (j >= 0
                   && rate_helper_maturity(helpers[order[static_cast<u64>(j)]]) > key_date) {
                order[static_cast<u64>(j + 1)] = order[static_cast<u64>(j)];
                j--;
            }
            order[static_cast<u64>(j + 1)] = key;
        }

        // -- Step 2: Sequential bootstrapping --
        // Working arrays for the partial curve under construction.
        // Start empty; vectors grow via push() as pillars are added.
        Vec<f64> pillar_t{};       // times in years from reference_date_
        Vec<f64> pillar_logdf{};   // -log(df) — positive values for interpolation stability
        Vec<Date> pillar_dates{};
        Vec<f64> pillar_dfs{};

        for (u64 k = 0; k < n_helpers; k++) {
            u64 idx = order[k];
            const RateHelper& helper = helpers[idx];
            Date mat = rate_helper_maturity(helper);

            // Skip if maturity is on or before reference date
            if (mat <= reference_date) continue;

            // Skip if this maturity is already a pillar (overlapping instrument)
            bool duplicate = false;
            for (u64 p = 0; p < pillar_dates.length(); p++) {
                if (pillar_dates[p] == mat) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            // Build df_lookup from currently bootstrapped pillars.
            // Uses linear interpolation of -log(df) between pillar times.
            auto df_at = [&](Date d) -> f64 {
                if (d <= reference_date) return 1.0;
                f64 t = dc.year_fraction(reference_date, d);
                u64 np = pillar_t.length();

                // No pillars yet: flat extrapolation at df=1.0
                if (np == 0) return 1.0;

                // Single pillar: flat extrapolation
                if (np == 1) return Math::exp(-pillar_logdf[0]);

                // Flat extrapolation beyond bounds
                if (t <= pillar_t[0])   return Math::exp(-pillar_logdf[0]);
                if (t >= pillar_t[np - 1]) return Math::exp(-pillar_logdf[np - 1]);

                // Binary search for interpolation segment
                u64 lo = 0, hi = np - 1;
                while (hi - lo > 1) {
                    u64 mid = (lo + hi) / 2;
                    if (pillar_t[mid] <= t) lo = mid;
                    else                     hi = mid;
                }

                // Linear interpolation of -log(df)
                f64 h_seg = pillar_t[hi] - pillar_t[lo];
                f64 w = (t - pillar_t[lo]) / h_seg;
                f64 interp_neg_log_df =
                    pillar_logdf[lo] + w * (pillar_logdf[hi] - pillar_logdf[lo]);
                return Math::exp(-interp_neg_log_df);
            };

            // Compute implied df at maturity using the helper's formula.
            // Dispatch via Variant::match — zero virtual function overhead.
            f64 df_mat = 1.0;
            helper.match(Overload{
                [&](const DepositRateHelper& h) {
                    df_mat = h.implied_df(reference_date, dc, df_at);
                },
                [&](const FraRateHelper& h) {
                    df_mat = h.implied_df(reference_date, dc, df_at);
                },
                [&](const SwapRateHelper& h) {
                    df_mat = h.implied_df(reference_date, dc, df_at);
                },
            });

            // Numerical safety: clamp df to reasonable range
            if (df_mat <= 0.0) df_mat = Limits<f64>::epsilon();
            if (df_mat > 1.0) df_mat = 1.0;

            // Add new pillar
            f64 t_mat = dc.year_fraction(reference_date, mat);
            pillar_t.push(t_mat);
            pillar_logdf.push(-::log(df_mat));
            pillar_dates.push(mat);
            pillar_dfs.push(df_mat);
        }

        // -- Step 3: Transfer working arrays to the curve --
        curve.pillar_dates_ = move(pillar_dates);
        curve.discount_factors_ = move(pillar_dfs);

        // -- Step 4: Build final interpolation --
        u64 np = pillar_t.length();
        if (np == 0) {
            // No pillars built: use empty interpolation.
            // discount_impl() detects 0 pillars and returns 1.0 directly.
            curve.log_df_interp_ = InterpType::linear(Vec<f64>{}, Vec<f64>{});
        } else if (np == 1) {
            // Single pillar: linear interpolation is the only valid choice.
            Vec<f64> t1 = Vec<f64>::make(1);
            Vec<f64> v1 = Vec<f64>::make(1);
            t1[0] = pillar_t[0];
            v1[0] = pillar_logdf[0];
            curve.log_df_interp_ = InterpType::linear(move(t1), move(v1));
        } else {
            // Multiple pillars: copy times and log-df values.
            Vec<f64> t_copy = Vec<f64>::make(np);
            Vec<f64> v_copy = Vec<f64>::make(np);
            for (u64 i = 0; i < np; i++) {
                t_copy[i] = pillar_t[i];
                v_copy[i] = pillar_logdf[i];
            }

            // [UNSPECIFIED] Interpolation method for discount factors.
            // Cubic spline is used for smoothness; monotonic cubic may be
            // more appropriate for arbitrage-free curve construction.
            if constexpr (requires { InterpType::cubic_spline(Vec<f64>{}, Vec<f64>{}); }) {
                curve.log_df_interp_ = InterpType::cubic_spline(move(t_copy), move(v_copy));
            } else {
                curve.log_df_interp_ = InterpType::linear(move(t_copy), move(v_copy));
            }
        }

        return curve;
    }

    // ----------------------------------------------------------
    // CRTP implementation for TermStructure
    // ----------------------------------------------------------

    [[nodiscard]] Date max_date_impl() const noexcept {
        u64 n = pillar_dates_.length();
        if (n == 0) return Base::reference_date_;
        return pillar_dates_[n - 1];
    }

    /// Discount factor at time t (years from reference_date_).
    /// Interpolates log-discount-factor and exponentiates.
    /// log_df_interp_ stores y_i = -log(df_i); df(t) = exp(-y(t)).
    [[nodiscard]] f64 discount_impl(f64 t) const noexcept {
        if (t <= 0.0) return 1.0;
        u64 n = pillar_dates_.length();
        if (n == 0) return 1.0;
        f64 neg_log_df = log_df_interp_(t);
        return Math::exp(-neg_log_df);
    }

    // ----------------------------------------------------------
    // Accessors
    // ----------------------------------------------------------

    [[nodiscard]] u64 pillar_count() const noexcept {
        return pillar_dates_.length();
    }

    [[nodiscard]] Date pillar_date(u64 i) const noexcept {
        return pillar_dates_[i];
    }

    [[nodiscard]] f64 pillar_df(u64 i) const noexcept {
        return discount_factors_[i];
    }

    /// Forward discount factor between two pillar indices.
    /// Returns df(i) / df(j).
    [[nodiscard]] f64 pillar_forward_df(u64 i, u64 j) const noexcept {
        if (discount_factors_[j] <= 0.0) return 1.0;
        return discount_factors_[i] / discount_factors_[j];
    }

    /// Simply-compounded forward rate between two pillar indices.
    [[nodiscard]] f64 pillar_forward_rate(u64 i, u64 j) const noexcept {
        f64 fwd_df = pillar_forward_df(i, j);
        f64 yf = Base::day_counter_.year_fraction(pillar_dates_[i], pillar_dates_[j]);
        if (yf <= 0.0) return 0.0;
        return (1.0 / fwd_df - 1.0) / yf;
    }
};

// Reflection (must be at namespace scope, not inside the struct body)
template<typename InterpType>
SPP_TEMPLATE_RECORD(PiecewiseYieldCurve, InterpType,
                    SPP_FIELD(pillar_dates_), SPP_FIELD(discount_factors_));

} // namespace spp::quant
