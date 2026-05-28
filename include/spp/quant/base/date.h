#pragma once

#include <spp/core/base.h>
#include <spp/core/tuple.h>
#include <chrono>

namespace spp::quant {

enum struct Weekday : u8 {
    Monday = 1,
    Tuesday = 2,
    Wednesday = 3,
    Thursday = 4,
    Friday = 5,
    Saturday = 6,
    Sunday = 7
};

struct Date {
    i32 serial_ = 0;

    static Date today() noexcept {
        auto now = std::chrono::system_clock::now();
        auto days = std::chrono::floor<std::chrono::days>(now);
        i32 civil_days = static_cast<i32>(days.time_since_epoch().count());
        return Date{civil_days + 25569};
    }

    static Date from_ymd(i32 y, u32 m, u32 d) noexcept {
        y -= static_cast<i32>(m <= 2);
        i32 era = (y >= 0 ? y : y - 399) / 400;
        u32 yoe = static_cast<u32>(y - era * 400);
        u32 mp = m + (m > 2 ? static_cast<u32>(-3) : 9u);
        u32 doy = (153u * mp + 2u) / 5u + d - 1u;
        u32 doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
        return Date{era * 146097 + static_cast<i32>(doe) - 719468 + 25569};
    }

    static Date min_date() noexcept {
        return Date{-53689};
    }
    static Date max_date() noexcept {
        return Date{2958465};
    }

    spp::Tuple<i32, u32, u32> ymd() const noexcept {
        i32 z = serial_ - 25569;
        z += 719468;
        i32 era = (z >= 0 ? z : z - 146096) / 146097;
        u32 doe = static_cast<u32>(z - era * 146097);
        u32 yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
        i32 y = static_cast<i32>(yoe) + era * 400;
        u32 doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
        u32 mp = (5u * doy + 2u) / 153u;
        u32 dy = doy - (153u * mp + 2u) / 5u + 1u;
        u32 mo = mp + (mp < 10u ? 3u : static_cast<u32>(-9));
        return spp::Tuple<i32, u32, u32>{y + static_cast<i32>(mo <= 2), mo, dy};
    }

    Weekday day_of_week() const noexcept {
        return static_cast<Weekday>(static_cast<u32>((serial_ + 5) % 7) + 1u);
    }

    u32 day_of_month() const noexcept {
        return ymd().get<2>();
    }

    u32 month() const noexcept {
        return ymd().get<1>();
    }

    i32 year() const noexcept {
        return ymd().get<0>();
    }

    bool is_weekend() const noexcept {
        Weekday wd = day_of_week();
        return wd == Weekday::Saturday || wd == Weekday::Sunday;
    }

    static bool is_leap(i32 year) noexcept {
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

    static u32 days_in_month(i32 year, u32 month) noexcept {
        static const u8 mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if(month == 2 && is_leap(year)) return 29;
        assert(month >= 1 && month <= 12);
        return mdays[month - 1];
    }

    u32 day_of_year() const noexcept {
        spp::Tuple<i32, u32, u32> ymd_ = ymd();
        i32 yy = ymd_.get<0>();
        u32 mm = ymd_.get<1>();
        u32 dd = ymd_.get<2>();
        static const u16 cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
        u32 doy = cum[mm - 1] + dd;
        if(mm > 2 && is_leap(yy)) doy++;
        return doy;
    }

    Date end_of_month() const noexcept {
        spp::Tuple<i32, u32, u32> ymd_ = ymd();
        i32 yy = ymd_.get<0>();
        u32 mm = ymd_.get<1>();
        return Date::from_ymd(yy, mm, days_in_month(yy, mm));
    }

    bool is_end_of_month() const noexcept {
        return day_of_month() == days_in_month(year(), month());
    }

    bool operator==(const Date& other) const noexcept {
        return serial_ == other.serial_;
    }
    bool operator!=(const Date& other) const noexcept {
        return serial_ != other.serial_;
    }
    bool operator<(const Date& other) const noexcept {
        return serial_ < other.serial_;
    }
    bool operator<=(const Date& other) const noexcept {
        return serial_ <= other.serial_;
    }
    bool operator>(const Date& other) const noexcept {
        return serial_ > other.serial_;
    }
    bool operator>=(const Date& other) const noexcept {
        return serial_ >= other.serial_;
    }

    Date& operator+=(i32 days) noexcept {
        serial_ += days;
        return *this;
    }
    Date& operator-=(i32 days) noexcept {
        serial_ -= days;
        return *this;
    }

    Date operator+(i32 days) const noexcept {
        return Date{static_cast<i32>(serial_ + days)};
    }
    Date operator-(i32 days) const noexcept {
        return Date{static_cast<i32>(serial_ - days)};
    }
    i32 operator-(Date other) const noexcept {
        return serial_ - other.serial_;
    }

    Date& operator++() noexcept {
        ++serial_;
        return *this;
    }
    Date operator++(int) noexcept {
        Date tmp = *this;
        ++serial_;
        return tmp;
    }
    Date& operator--() noexcept {
        --serial_;
        return *this;
    }
    Date operator--(int) noexcept {
        Date tmp = *this;
        --serial_;
        return tmp;
    }
};

inline Date operator+(i32 days, Date d) noexcept {
    return d + days;
}

inline Date operator-(i32 days, Date d) noexcept {
    return d - days;
}

enum struct Frequency : u16 {
    Once = 0,
    Annual = 1,
    SemiAnnual = 2,
    Quarterly = 4,
    Monthly = 12,
    Weekly = 52,
    Daily = 365
};

constexpr u8 frequency_months(Frequency f) noexcept {
    switch(f) {
    case Frequency::Annual: return 12;
    case Frequency::SemiAnnual: return 6;
    case Frequency::Quarterly: return 3;
    case Frequency::Monthly: return 1;
    default: return 0;
    }
}

constexpr i32 frequency_days(Frequency f) noexcept {
    switch(f) {
    case Frequency::Daily: return 1;
    case Frequency::Weekly: return 7;
    default: return 0;
    }
}

enum struct Compounding : u8 {
    Simple = 0,
    Compounded = 1,
    Continuous = 2,
    SimpleThenCompounded = 3
};

struct Period {
    i32 length_ = 0;
    Frequency unit_ = Frequency::Once;
};

inline Date add_period(Date d, Period p) noexcept {
    if(p.unit_ == Frequency::Once || p.length_ == 0) return d;

    if(p.unit_ == Frequency::Daily) return d + p.length_;

    if(p.unit_ == Frequency::Weekly) {
        return d + (p.length_ * 7);
    }

    if(p.unit_ == Frequency::Monthly || p.unit_ == Frequency::Quarterly ||
       p.unit_ == Frequency::SemiAnnual || p.unit_ == Frequency::Annual) {
        u8 months_per = frequency_months(p.unit_);
        i32 total_months = p.length_ * months_per;

        spp::Tuple<i32, u32, u32> ymd_ = d.ymd();
        i32 yy = ymd_.get<0>();
        u32 mm = ymd_.get<1>();
        u32 dd = ymd_.get<2>();

        i32 new_y = yy;
        i32 new_m = static_cast<i32>(mm) + total_months;

        while(new_m > 12) {
            new_m -= 12;
            new_y++;
        }
        while(new_m < 1) {
            new_m += 12;
            new_y--;
        }

        u32 max_d = Date::days_in_month(new_y, static_cast<u32>(new_m));
        u32 new_d = dd;
        if(new_d > max_d) new_d = max_d;

        return Date::from_ymd(new_y, static_cast<u32>(new_m), new_d);
    }

    return d;
}

inline Date operator+(Date d, Period p) noexcept {
    return add_period(d, p);
}

inline Date operator-(Date d, Period p) noexcept {
    Period neg = {-p.length_, p.unit_};
    return add_period(d, neg);
}

inline Date& operator+=(Date& d, Period p) noexcept {
    d = d + p;
    return d;
}

inline Date& operator-=(Date& d, Period p) noexcept {
    d = d - p;
    return d;
}

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::Date, "Date", SPP_FIELD(serial_));
SPP_NAMED_ENUM(::spp::quant::Weekday, "Weekday", Monday, SPP_CASE(Monday), SPP_CASE(Tuesday),
               SPP_CASE(Wednesday), SPP_CASE(Thursday), SPP_CASE(Friday), SPP_CASE(Saturday),
               SPP_CASE(Sunday));
SPP_NAMED_ENUM(::spp::quant::Frequency, "Frequency", Daily, SPP_CASE(Once), SPP_CASE(Annual),
               SPP_CASE(SemiAnnual), SPP_CASE(Quarterly), SPP_CASE(Monthly), SPP_CASE(Weekly),
               SPP_CASE(Daily));
SPP_NAMED_ENUM(::spp::quant::Compounding, "Compounding", Compounded, SPP_CASE(Simple),
               SPP_CASE(Compounded), SPP_CASE(Continuous), SPP_CASE(SimpleThenCompounded));
SPP_NAMED_RECORD(::spp::quant::Period, "Period", SPP_FIELD(length_), SPP_FIELD(unit_));
