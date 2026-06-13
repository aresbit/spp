#pragma once
#include <spp/core/base.h>

namespace spp::quant::data {

enum class Market_Type : u8 {
    unknown,
    stock_cn,
    future_cn,
    crypto,
    forex
};

enum class Frequency : u8 {
    tick,
    second_5,
    minute_1,
    minute_5,
    minute_15,
    minute_30,
    hour_1,
    day_1,
    week_1,
    month_1
};

using Price = Decimal<8>;
using Volume = f64;
using Timestamp = Deterministic_Time;
using Duration = Deterministic_Duration;

[[nodiscard]] inline f64 price_to_f64(Price p) noexcept {
    return static_cast<f64>(p.raw()) / static_cast<f64>(Price::factor());
}

[[nodiscard]] inline Price f64_to_price(f64 v) noexcept {
    return Price::from_raw(static_cast<i64>(v * static_cast<f64>(Price::factor())));
}

[[nodiscard]] inline Frequency parse_frequency(String_View freq) noexcept {
    if(freq == "tick"_v) return Frequency::tick;
    if(freq == "5s"_v || freq == "second_5"_v) return Frequency::second_5;
    if(freq == "1min"_v || freq == "minute_1"_v) return Frequency::minute_1;
    if(freq == "5min"_v || freq == "minute_5"_v) return Frequency::minute_5;
    if(freq == "15min"_v || freq == "minute_15"_v) return Frequency::minute_15;
    if(freq == "30min"_v || freq == "minute_30"_v) return Frequency::minute_30;
    if(freq == "1h"_v || freq == "hour_1"_v) return Frequency::hour_1;
    if(freq == "1d"_v || freq == "day_1"_v) return Frequency::day_1;
    if(freq == "1w"_v || freq == "week_1"_v) return Frequency::week_1;
    if(freq == "1M"_v || freq == "month_1"_v) return Frequency::month_1;
    return Frequency::minute_1;
}

[[nodiscard]] inline String_View frequency_name(Frequency freq) noexcept {
    switch(freq) {
    case Frequency::tick:       return "tick"_v;
    case Frequency::second_5:   return "5s"_v;
    case Frequency::minute_1:   return "1min"_v;
    case Frequency::minute_5:   return "5min"_v;
    case Frequency::minute_15:  return "15min"_v;
    case Frequency::minute_30:  return "30min"_v;
    case Frequency::hour_1:     return "1h"_v;
    case Frequency::day_1:      return "1d"_v;
    case Frequency::week_1:     return "1w"_v;
    case Frequency::month_1:    return "1M"_v;
    }
    return "1min"_v;
}

[[nodiscard]] inline Deterministic_Duration frequency_duration(Frequency freq) noexcept {
    using D = Deterministic_Duration;
    switch(freq) {
    case Frequency::tick:       return D::from_ns(0);
    case Frequency::second_5:   return D::from_s(5);
    case Frequency::minute_1:   return D::from_s(60);
    case Frequency::minute_5:   return D::from_s(300);
    case Frequency::minute_15:  return D::from_s(900);
    case Frequency::minute_30:  return D::from_s(1800);
    case Frequency::hour_1:     return D::from_s(3600);
    case Frequency::day_1:      return D::from_s(86400);
    case Frequency::week_1:     return D::from_s(604800);
    case Frequency::month_1:    return D::from_s(2592000);
    }
    return D::from_s(60);
}

namespace detail {

[[nodiscard]] constexpr inline u64 truncate_to_bucket(u64 unix_ns, u64 bucket_ns) noexcept {
    if(bucket_ns == 0) return unix_ns;
    return (unix_ns / bucket_ns) * bucket_ns;
}

} // namespace detail

} // namespace spp::quant::data

SPP_ENUM(spp::quant::data::Market_Type, unknown,
    SPP_CASE(stock_cn), SPP_CASE(future_cn), SPP_CASE(crypto), SPP_CASE(forex));

SPP_ENUM(spp::quant::data::Frequency, minute_1,
    SPP_CASE(tick), SPP_CASE(second_5), SPP_CASE(minute_1), SPP_CASE(minute_5),
    SPP_CASE(minute_15), SPP_CASE(minute_30), SPP_CASE(hour_1), SPP_CASE(day_1),
    SPP_CASE(week_1), SPP_CASE(month_1));

namespace spp::Hash {

template<>
struct Hash<Deterministic_Time> {
    [[nodiscard]] constexpr static u64 hash(Deterministic_Time t) noexcept {
        return Hash<i64>::hash(t.unix_ns());
    }
};

template<>
struct Hash<Deterministic_Duration> {
    [[nodiscard]] constexpr static u64 hash(Deterministic_Duration d) noexcept {
        return Hash<i64>::hash(d.ns());
    }
};

} // namespace spp::Hash
