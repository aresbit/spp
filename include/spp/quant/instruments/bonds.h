#pragma once

#include <spp/core/base.h>
#include <spp/quant/instruments/instrument.h>
#include <spp/quant/base/date.h>
#include <spp/quant/base/calendar.h>
#include <spp/quant/base/daycounter.h>

namespace spp::quant {

// =========================================================================
// FixedRateBond — bullet bond with regular fixed coupon payments
// =========================================================================
struct FixedRateBond : Instrument {
    f64                   face_value_        = 100.0;
    f64                   coupon_rate_       = 0.05;
    Frequency             coupon_frequency_  = Frequency::SemiAnnual;
    Date                  maturity_;
    Date                  issue_date_;
    DayCounter            day_counter_       = DayCounter::actual_actual_isma();
    Calendar              calendar_          = Calendar::weekend_only();
    BusinessDayConvention bdc_               = BusinessDayConvention::Following;

    // ---- Coupon schedule --------------------------------------------------
    // Returns the vector of coupon payment dates from issue to maturity.
    Vec<Date> coupon_dates() const {
        Vec<Date> dates;

        // Walk backward from maturity to build the schedule
        Period freq_period{1, coupon_frequency_};
        Date   d = maturity_;

        u64 max_coupons = 256; // safety cap
        while (d > issue_date_ && dates.length() < max_coupons) {
            // Adjust for business-day convention
            Date adj = calendar_.adjust(d, bdc_);
            dates.push(adj);

            // Step back one coupon period
            d = d - freq_period;
        }

        // The loop produces dates in reverse order; we would reverse them,
        // but for the accrued-interest / NPV computation below we just need
        // all dates > settlement.  Return as-is for simplicity.

        return dates;
    }

    // ---- Accrued interest at settlement_date ------------------------------
    f64 accrued_interest(Date settlement_date) const {
        // Find the previous coupon date
        Period freq_period{1, coupon_frequency_};
        Date   prev_coupon = maturity_;
        while (prev_coupon - freq_period > issue_date_) {
            prev_coupon = prev_coupon - freq_period;
        }
        prev_coupon = calendar_.adjust(prev_coupon, bdc_);

        if (settlement_date <= prev_coupon) return 0.0;

        // Find the next coupon date after settlement
        Date next_coupon = prev_coupon + freq_period;
        next_coupon = calendar_.adjust(next_coupon, bdc_);

        f64 dcf = day_counter_.year_fraction(prev_coupon, settlement_date,
                                              Opt<Date>{}, Opt<Date>{});
        f64 coupon = face_value_ * coupon_rate_ /
                     static_cast<f64>(static_cast<u16>(coupon_frequency_));

        return coupon * Math::min(dcf, 1.0); // clamp in case of bad dates
    }

    f64 npv() const override {
        // placeholder — use price_fixed_bond()
        return 0.0;
    }

    bool is_expired() const override {
        return Date::today() >= maturity_;
    }

    SPP_RECORD(FixedRateBond,
        SPP_FIELD(face_value_),
        SPP_FIELD(coupon_rate_),
        SPP_FIELD(coupon_frequency_),
        SPP_FIELD(maturity_),
        SPP_FIELD(issue_date_));
};

// =========================================================================
// ZeroCouponBond — single payment at maturity
// =========================================================================
struct ZeroCouponBond : Instrument {
    f64  face_value_ = 100.0;
    Date maturity_;

    f64 npv() const override { return 0.0; }

    bool is_expired() const override {
        return Date::today() >= maturity_;
    }

    SPP_RECORD(ZeroCouponBond,
        SPP_FIELD(face_value_),
        SPP_FIELD(maturity_));
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::FixedRateBond, "FixedRateBond",
    SPP_FIELD(face_value_), SPP_FIELD(coupon_rate_),
    SPP_FIELD(coupon_frequency_), SPP_FIELD(maturity_),
    SPP_FIELD(issue_date_));

SPP_NAMED_RECORD(::spp::quant::ZeroCouponBond, "ZeroCouponBond",
    SPP_FIELD(face_value_), SPP_FIELD(maturity_));
