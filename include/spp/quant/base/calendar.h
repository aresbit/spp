#pragma once

#include <spp/core/base.h>
#include <spp/quant/base/date.h>

namespace spp::quant {

enum struct BusinessDayConvention : u8 {
    Following = 0,
    ModifiedFollowing = 1,
    Preceding = 2,
    ModifiedPreceding = 3,
    Unadjusted = 4
};

struct Calendar {
    Vec<Date> holidays_;

    Calendar() = default;

    void add_holiday(Date d) noexcept {
        holidays_.push(d);
    }

    bool is_holiday(Date d) const noexcept {
        for(u64 i = 0; i < holidays_.length(); i++) {
            if(holidays_[i] == d) return true;
        }
        return false;
    }

    bool is_weekend(Date d) const noexcept {
        return d.is_weekend();
    }

    bool is_business_day(Date d) const noexcept {
        return !is_weekend(d) && !is_holiday(d);
    }

    Date adjust(Date d, BusinessDayConvention conv) const noexcept {
        switch(conv) {
        case BusinessDayConvention::Unadjusted:
            return d;

        case BusinessDayConvention::Following: {
            Date result = d;
            while(!is_business_day(result)) result = result + 1;
            return result;
        }

        case BusinessDayConvention::ModifiedFollowing: {
            Date result = d;
            while(!is_business_day(result)) result = result + 1;
            if(result.month() != d.month()) {
                result = d;
                while(!is_business_day(result)) result = result - 1;
            }
            return result;
        }

        case BusinessDayConvention::Preceding: {
            Date result = d;
            while(!is_business_day(result)) result = result - 1;
            return result;
        }

        case BusinessDayConvention::ModifiedPreceding: {
            Date result = d;
            while(!is_business_day(result)) result = result - 1;
            if(result.month() != d.month()) {
                result = d;
                while(!is_business_day(result)) result = result + 1;
            }
            return result;
        }
        }

        return d;
    }

    Date advance(Date d, i32 n, BusinessDayConvention conv) const noexcept {
        if(n == 0) return adjust(d, conv);

        Date result = d;
        if(n > 0) {
            while(n > 0) {
                result = result + 1;
                if(is_business_day(result)) n--;
            }
        } else {
            while(n < 0) {
                result = result - 1;
                if(is_business_day(result)) n++;
            }
        }
        return result;
    }

    static Calendar us_settlement() noexcept {
        Calendar cal;
        return cal;
    }

    static Calendar uk_settlement() noexcept {
        Calendar cal;
        return cal;
    }

    static Calendar target2() noexcept {
        Calendar cal;
        return cal;
    }

    static Calendar china_ib() noexcept {
        Calendar cal;
        return cal;
    }

    static Calendar weekend_only() noexcept {
        Calendar cal;
        return cal;
    }
};

} // namespace spp::quant

SPP_NAMED_ENUM(::spp::quant::BusinessDayConvention, "BusinessDayConvention", Following,
               SPP_CASE(Following), SPP_CASE(ModifiedFollowing), SPP_CASE(Preceding),
               SPP_CASE(ModifiedPreceding), SPP_CASE(Unadjusted));

SPP_NAMED_RECORD(::spp::quant::Calendar, "Calendar", SPP_FIELD(holidays_));
