#pragma once

#include <spp/core/base.h>
#include <spp/core/tuple.h>
#include <spp/quant/base/date.h>

namespace spp::quant {

using YF_Fn = f64 (*)(Date, Date, Opt<Date>, Opt<Date>);

namespace detail {

inline f64 yf_actual_360(Date start, Date end, Opt<Date>, Opt<Date>) noexcept {
    return static_cast<f64>(end.serial_ - start.serial_) / 360.0;
}

inline f64 yf_actual_365_fixed(Date start, Date end, Opt<Date>, Opt<Date>) noexcept {
    return static_cast<f64>(end.serial_ - start.serial_) / 365.0;
}

inline f64 yf_actual_actual_isma(Date start, Date end, Opt<Date> ref_start,
                                  Opt<Date> ref_end) noexcept {
    i32 days = end.serial_ - start.serial_;
    if(days == 0) return 0.0;

    if(ref_start.ok() && ref_end.ok()) {
        f64 reference_days = static_cast<f64>((*ref_end).serial_ - (*ref_start).serial_);
        if(reference_days > 0.0) {
            return static_cast<f64>(days) / reference_days;
        }
    }

    spp::Tuple<i32, u32, u32> s_ymd = start.ymd();
    i32 sy = s_ymd.get<0>();
    spp::Tuple<i32, u32, u32> e_ymd = end.ymd();
    i32 ey = e_ymd.get<0>();

    if(sy == ey) {
        f64 denom = Date::is_leap(sy) ? 366.0 : 365.0;
        return static_cast<f64>(days) / denom;
    }

    Date year_end_s = Date::from_ymd(sy, 12, 31);
    i32 days_s = year_end_s.serial_ - start.serial_ + 1;
    f64 frac_s = static_cast<f64>(days_s) / (Date::is_leap(sy) ? 366.0 : 365.0);

    Date year_start_e = Date::from_ymd(ey, 1, 1);
    i32 days_e = end.serial_ - year_start_e.serial_;
    f64 frac_e = static_cast<f64>(days_e) / (Date::is_leap(ey) ? 366.0 : 365.0);

    f64 frac_mid = 0.0;
    for(i32 y = sy + 1; y < ey; y++) {
        frac_mid += 1.0;
    }

    return frac_s + frac_mid + frac_e;
}

inline f64 yf_thirty_360_usa(Date start, Date end, Opt<Date>, Opt<Date>) noexcept {
    spp::Tuple<i32, u32, u32> yms = start.ymd();
    spp::Tuple<i32, u32, u32> yme = end.ymd();

    i32 d1 = static_cast<i32>(yms.get<2>());
    i32 d2 = static_cast<i32>(yme.get<2>());
    i32 m1 = static_cast<i32>(yms.get<1>());
    i32 m2 = static_cast<i32>(yme.get<1>());
    i32 y1 = yms.get<0>();
    i32 y2 = yme.get<0>();

    if(d1 == 31)
        d1 = 30;
    if(d2 == 31 && d1 >= 30)
        d2 = 30;

    return static_cast<f64>(360 * (y2 - y1) + 30 * (m2 - m1) + (d2 - d1)) / 360.0;
}

inline f64 yf_thirty_360_european(Date start, Date end, Opt<Date>, Opt<Date>) noexcept {
    spp::Tuple<i32, u32, u32> yms = start.ymd();
    spp::Tuple<i32, u32, u32> yme = end.ymd();

    i32 d1 = static_cast<i32>(yms.get<2>());
    i32 d2 = static_cast<i32>(yme.get<2>());
    i32 m1 = static_cast<i32>(yms.get<1>());
    i32 m2 = static_cast<i32>(yme.get<1>());
    i32 y1 = yms.get<0>();
    i32 y2 = yme.get<0>();

    if(d1 == 31)
        d1 = 30;
    if(d2 == 31)
        d2 = 30;

    return static_cast<f64>(360 * (y2 - y1) + 30 * (m2 - m1) + (d2 - d1)) / 360.0;
}

} // namespace detail

struct DayCounter {
    YF_Fn yf_fn_ = detail::yf_actual_360;

    constexpr DayCounter() = default;

    constexpr explicit DayCounter(YF_Fn fn) noexcept : yf_fn_(fn) {
    }

    i64 day_count(Date start, Date end) const noexcept {
        return static_cast<i64>(end.serial_ - start.serial_);
    }

    f64 year_fraction(Date start, Date end, Opt<Date> ref_start = {},
                      Opt<Date> ref_end = {}) const noexcept {
        return yf_fn_(start, end, ref_start, ref_end);
    }

    static DayCounter actual_360() noexcept {
        return DayCounter{detail::yf_actual_360};
    }

    static DayCounter actual_365_fixed() noexcept {
        return DayCounter{detail::yf_actual_365_fixed};
    }

    static DayCounter actual_actual_isma() noexcept {
        return DayCounter{detail::yf_actual_actual_isma};
    }

    static DayCounter thirty_360_usa() noexcept {
        return DayCounter{detail::yf_thirty_360_usa};
    }

    static DayCounter thirty_360_european() noexcept {
        return DayCounter{detail::yf_thirty_360_european};
    }

    bool operator==(const DayCounter& other) const noexcept {
        return yf_fn_ == other.yf_fn_;
    }
    bool operator!=(const DayCounter& other) const noexcept {
        return yf_fn_ != other.yf_fn_;
    }
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::DayCounter, "DayCounter", SPP_FIELD(yf_fn_));
