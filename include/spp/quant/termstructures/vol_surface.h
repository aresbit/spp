#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h instead."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/base/daycounter.h"
#include "spp/quant/math/interpolation.h"
#include <cmath>

namespace spp::quant {

// ============================================================
// BlackVolSurface — Black implied volatility surface
// ============================================================
//
// Represents the function sigma(T, K) where:
//   T     = time to expiry (in years from reference_date_)
//   K     = strike price
//   sigma = Black-Scholes implied volatility
//
// The surface supports 2D interpolation:
//   1. Along strikes: per-expiry cubic spline interpolation of vol vs strike.
//   2. Across expiries: linear interpolation in total variance (sigma^2 * T)
//      between the two nearest expiries.
//
// Extrapolation beyond the surface bounds is flat (constant vol).
//
// Template parameter A controls the allocator for internal storage.

template<typename A = Mdefault>
struct BlackVolSurface {
    Date reference_date_;
    DayCounter day_counter_;

    // Per-expiry data (all vectors indexed by expiry, sorted by expiry date)
    Vec<Date, A> expiries_;                    // sorted expiry dates
    Vec<Vec<f64, A>, A> strike_grids_;        // strikes per expiry (sorted ascending)
    Vec<Vec<f64, A>, A> vol_grids_;           // implied vols per expiry

    // Per-expiry strike interpolation: vol vs strike for each expiry
    Vec<interp::Interpolation<A>, A> strike_interps_;

    // ATM vol interpolation across expiries (used as fallback for time interp)
    interp::Interpolation<A> atm_time_interp_;

    // ----------------------------------------------------------
    // Factory: build from market data
    // ----------------------------------------------------------
    //
    // expiries: sorted list of expiry dates
    // strikes:  per-expiry list of strike levels (each sorted ascending)
    // vols:     per-expiry list of Black implied volatilities
    //
    // The i-th vectors in strikes and vols correspond to expiries[i].
    // All strike grids and vol grids must have the same length per expiry.

    [[nodiscard]] static BlackVolSurface from_market(
        Date reference_date,
        Vec<Date, A> expiries,
        Vec<Vec<f64, A>, A> strikes,
        Vec<Vec<f64, A>, A> vols,
        DayCounter dc = DayCounter::actual_365_fixed()) noexcept {

        BlackVolSurface surface;
        surface.reference_date_ = reference_date;
        surface.day_counter_ = dc;

        u64 n_exp = expiries.length();
        if (n_exp == 0) return surface;

        // Sort expiries and reorder strike/vol grids accordingly.
        // We use insertion sort on a small index array.
        Vec<u64, A> order = Vec<u64, A>::make(n_exp);
        for (u64 i = 0; i < n_exp; i++) {
            order[i] = i;
        }

        // Sort by expiry date ascending
        for (u64 i = 1; i < n_exp; i++) {
            u64 key = order[i];
            Date key_date = expiries[key];
            i64 j = static_cast<i64>(i) - 1;
            while (j >= 0 && expiries[order[static_cast<u64>(j)]] > key_date) {
                order[static_cast<u64>(j + 1)] = order[static_cast<u64>(j)];
                j--;
            }
            order[static_cast<u64>(j + 1)] = key;
        }

        // Reorder into sorted arrays
        surface.expiries_ = Vec<Date, A>::make(n_exp);
        surface.strike_grids_ = Vec<Vec<f64, A>, A>::make(n_exp);
        surface.vol_grids_ = Vec<Vec<f64, A>, A>::make(n_exp);
        for (u64 i = 0; i < n_exp; i++) {
            u64 src = order[i];
            surface.expiries_[i] = expiries[src];
            // Move the strike and vol grids
            surface.strike_grids_[i] = move(strikes[src]);
            surface.vol_grids_[i] = move(vols[src]);
        }

        // Build per-expiry strike interpolations (cubic spline for smooth smile)
        surface.strike_interps_ = Vec<interp::Interpolation<A>, A>::make(n_exp);
        Vec<f64, A> atm_times = Vec<f64, A>::make(n_exp);
        Vec<f64, A> atm_vols = Vec<f64, A>::make(n_exp);

        for (u64 i = 0; i < n_exp; i++) {
            u64 n_strikes = surface.strike_grids_[i].length();
            if (n_strikes == 0) continue;

            // Copy strike and vol data for interpolation constructor
            Vec<f64, A> ks = Vec<f64, A>::make(n_strikes);
            Vec<f64, A> vs = Vec<f64, A>::make(n_strikes);
            for (u64 j = 0; j < n_strikes; j++) {
                ks[j] = surface.strike_grids_[i][j];
                vs[j] = surface.vol_grids_[i][j];
            }

            if (n_strikes == 1) {
                surface.strike_interps_[i] = interp::Interpolation<A>::linear(move(ks), move(vs));
            } else if (n_strikes == 2) {
                surface.strike_interps_[i] = interp::Interpolation<A>::linear(move(ks), move(vs));
            } else {
                // [UNSPECIFIED] Volatility smile interpolation method.
                // Cubic spline is used for smoothness; monotonic cubic may be
                // preferable to avoid arbitrage violations in the vol surface.
                if constexpr (requires {
                        interp::Interpolation<A>::cubic_spline(Vec<f64, A>{}, Vec<f64, A>{});
                    }) {
                    surface.strike_interps_[i] =
                        interp::Interpolation<A>::cubic_spline(move(ks), move(vs));
                } else {
                    surface.strike_interps_[i] =
                        interp::Interpolation<A>::linear(move(ks), move(vs));
                }
            }

            // ATM vol = vol at the middle strike
            u64 mid = n_strikes / 2;
            atm_times[i] = dc.year_fraction(reference_date, surface.expiries_[i]);
            atm_vols[i] = surface.vol_grids_[i][mid];
        }

        // Build time interpolation of ATM vol (linear in vol vs time)
        if (n_exp == 1) {
            surface.atm_time_interp_ =
                interp::Interpolation<A>::linear(move(atm_times), move(atm_vols));
        } else if (n_exp >= 2) {
            surface.atm_time_interp_ =
                interp::Interpolation<A>::linear(move(atm_times), move(atm_vols));
        }

        return surface;
    }

    // ----------------------------------------------------------
    // Query: Black implied volatility at (expiry, strike)
    // ----------------------------------------------------------
    //
    // Algorithm:
    //   1. Convert expiry to time t = year_fraction(reference_date_, expiry).
    //   2. Find the two bracketing expiries in the surface.
    //   3. For each bracketing expiry, interpolate vol vs strike at the
    //      requested strike.
    //   4. Interpolate the two resulting vols across time using linear
    //      interpolation in total variance (sigma^2 * t).
    //
    // Extrapolation: flat beyond the surface bounds in both dimensions.

    [[nodiscard]] f64 black_vol(Date expiry, f64 strike) const noexcept {
        f64 t = day_counter_.year_fraction(reference_date_, expiry);
        return black_vol_t(t, strike);
    }

    /// Query by time (years from reference_date_) instead of date.
    [[nodiscard]] f64 black_vol_t(f64 t, f64 strike) const noexcept {
        u64 n_exp = expiries_.length();
        if (n_exp == 0) return 0.0;

        // Convert expiries to times for comparison
        // (Pre-computing these would be more efficient; done here for simplicity.)

        if (n_exp == 1) {
            // Single expiry: only strike interpolation, no time interpolation
            if (strike_interps_[0].x_.length() == 0) return 0.0;
            return strike_interp_at(0, strike);
        }

        // Find bracketing expiries by time
        // Build times on the fly
        u64 idx_lo = 0;
        u64 idx_hi = n_exp - 1;

        // Handle extrapolation: t before first expiry
        f64 t0 = day_counter_.year_fraction(reference_date_, expiries_[0]);
        if (t <= t0) {
            return strike_interp_at(0, strike);
        }

        // Handle extrapolation: t after last expiry
        f64 t_last = day_counter_.year_fraction(reference_date_, expiries_[n_exp - 1]);
        if (t >= t_last) {
            return strike_interp_at(n_exp - 1, strike);
        }

        // Binary search for the bracketing expiries
        u64 lo = 0, hi = n_exp - 1;
        while (hi - lo > 1) {
            u64 mid = (lo + hi) / 2;
            f64 t_mid = day_counter_.year_fraction(reference_date_, expiries_[mid]);
            if (t_mid <= t) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        idx_lo = lo;
        idx_hi = hi;

        // Interpolate vol vs strike at each bracketing expiry
        f64 vol_lo = strike_interp_at(idx_lo, strike);
        f64 vol_hi = strike_interp_at(idx_hi, strike);

        // Time interpolation: linear in total variance (sigma^2 * t)
        f64 t_lo = day_counter_.year_fraction(reference_date_, expiries_[idx_lo]);
        f64 t_hi = day_counter_.year_fraction(reference_date_, expiries_[idx_hi]);

        f64 var_lo = vol_lo * vol_lo * t_lo;
        f64 var_hi = vol_hi * vol_hi * t_hi;

        f64 dt = t_hi - t_lo;
        if (dt <= 0.0) return vol_lo;

        f64 w = (t - t_lo) / dt;
        f64 var = var_lo + w * (var_hi - var_lo);

        if (var <= 0.0) return 0.0;
        return Math::sqrt(var / t);
    }

    // ----------------------------------------------------------
    // ATM volatility for a given expiry
    // ----------------------------------------------------------
    //
    // ATM is defined as the middle strike of the nearest expiry's
    // strike grid. The vol is then interpolated in time if needed.

    [[nodiscard]] f64 atm_vol(Date expiry) const noexcept {
        f64 t = day_counter_.year_fraction(reference_date_, expiry);
        u64 n_exp = expiries_.length();
        if (n_exp == 0) return 0.0;

        if (n_exp == 1) {
            // Use the middle strike
            u64 nk = strike_grids_[0].length();
            if (nk == 0) return 0.0;
            u64 mid = nk / 2;
            return vol_grids_[0][mid];
        }

        // Find nearest expiry and return its ATM vol
        // Binary search
        f64 t0 = day_counter_.year_fraction(reference_date_, expiries_[0]);
        if (t <= t0) {
            u64 nk = strike_grids_[0].length();
            if (nk == 0) return 0.0;
            return vol_grids_[0][nk / 2];
        }

        f64 t_last = day_counter_.year_fraction(reference_date_, expiries_[n_exp - 1]);
        if (t >= t_last) {
            u64 nk = strike_grids_[n_exp - 1].length();
            if (nk == 0) return 0.0;
            return vol_grids_[n_exp - 1][nk / 2];
        }

        u64 lo = 0, hi = n_exp - 1;
        while (hi - lo > 1) {
            u64 mid = (lo + hi) / 2;
            f64 t_mid = day_counter_.year_fraction(reference_date_, expiries_[mid]);
            if (t_mid <= t) {
                lo = mid;
            } else {
                hi = mid;
            }
        }

        // Interpolate ATM vols in time (linear in vol)
        f64 t_lo = day_counter_.year_fraction(reference_date_, expiries_[lo]);
        f64 t_hi = day_counter_.year_fraction(reference_date_, expiries_[hi]);
        f64 dt = t_hi - t_lo;
        if (dt <= 0.0) {
            u64 nk = strike_grids_[lo].length();
            if (nk == 0) return 0.0;
            return vol_grids_[lo][nk / 2];
        }

        u64 nk_lo = strike_grids_[lo].length();
        u64 nk_hi = strike_grids_[hi].length();
        if (nk_lo == 0 || nk_hi == 0) return 0.0;

        f64 atm_lo = vol_grids_[lo][nk_lo / 2];
        f64 atm_hi = vol_grids_[hi][nk_hi / 2];

        f64 w = (t - t_lo) / dt;
        return atm_lo + w * (atm_hi - atm_lo);
    }

    // ----------------------------------------------------------
    // Accessors for inspection
    // ----------------------------------------------------------

    /// Strike grid for a specific expiry index.
    [[nodiscard]] Slice<const f64> strikes_for_expiry(u64 idx) const noexcept {
        if (idx >= strike_grids_.length()) return Slice<const f64>{};
        const Vec<f64, A>& g = strike_grids_[idx];
        return Slice<const f64>{g.data(), g.length()};
    }

    /// Vol grid for a specific expiry index.
    [[nodiscard]] Slice<const f64> vols_for_expiry(u64 idx) const noexcept {
        if (idx >= vol_grids_.length()) return Slice<const f64>{};
        const Vec<f64, A>& g = vol_grids_[idx];
        return Slice<const f64>{g.data(), g.length()};
    }

    /// Number of expiries in the surface.
    [[nodiscard]] u64 expiry_count() const noexcept {
        return expiries_.length();
    }

    /// Expiry date at a given index.
    [[nodiscard]] Date expiry_date(u64 idx) const noexcept {
        return expiries_[idx];
    }

private:
    // ----------------------------------------------------------
    // Internal: interpolate vol vs strike at a given expiry index
    // ----------------------------------------------------------
    [[nodiscard]] f64 strike_interp_at(u64 idx, f64 strike) const noexcept {
        const interp::Interpolation<A>& interp = strike_interps_[idx];
        u64 n = interp.x_.length();
        if (n == 0) return 0.0;
        return interp(strike);
    }
};

// Reflection (must be at namespace scope, not inside the struct body)
template<typename A>
SPP_TEMPLATE_RECORD(BlackVolSurface, A,
                    SPP_FIELD(reference_date_), SPP_FIELD(day_counter_),
                    SPP_FIELD(expiries_), SPP_FIELD(strike_grids_), SPP_FIELD(vol_grids_));

} // namespace spp::quant
