#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/base/calendar.h"

namespace spp::quant {

// ---- DateGeneration rule ---------------------------------------------------
enum struct DateGeneration : u8 {
    Backward = 0,
    Forward = 1,
    ThirdWednesday = 2,
    Zero = 3,
    CDS = 4,
};

// ---- Schedule --------------------------------------------------------------
struct Schedule {
    Vec<Date> dates_;

    // ------ static factories -------------------------------------------------
    //
    // effective_date     – first date of the schedule
    // termination_date   – final date (adjusted by termination_bdc)
    // frequency          – payment / reset frequency
    // calendar           – holiday calendar for business-day adjustments
    // bdc                – business-day convention for intermediate dates
    // termination_bdc    – business-day convention for the termination date
    // rule               – date-generation rule (Backward / Forward / ...)
    // end_of_month       – force end-of-month for intermediate dates
    // first_date         – optional explicit first regular date
    // next_to_last_date  – optional explicit penultimate regular date
    static Schedule make(Date effective_date, Date termination_date,
                         Frequency frequency, const Calendar& calendar,
                         BusinessDayConvention bdc,
                         BusinessDayConvention termination_bdc,
                         DateGeneration rule, bool end_of_month = false,
                         Opt<Date> first_date = {},
                         Opt<Date> next_to_last_date = {});

    // Short-rate schedule (floating-leg accrual dates).
    static Schedule make_short_rate(Date effective_date, Date termination_date,
                                    Frequency frequency,
                                    const Calendar& calendar,
                                    BusinessDayConvention bdc);

    // ------ accessors --------------------------------------------------------
    Date operator[](u64 i) const { return dates_[i]; }
    u64  size()           const { return dates_.length(); }
    Date start_date()     const { return dates_.front(); }
    Date end_date()       const { return dates_.back(); }
    Slice<const Date> dates() const { return dates_.slice(); }

    Schedule after(u64 i) const {
        Schedule ret;
        for(u64 j = i; j < dates_.length(); j++) ret.dates_.push(dates_[j]);
        return ret;
    }
    Schedule until(u64 i) const {
        Schedule ret;
        for(u64 j = 0; j <= i && j < dates_.length(); j++)
            ret.dates_.push(dates_[j]);
        return ret;
    }

};

// =========================================================================
// detail helpers — not part of the public API
// =========================================================================
namespace detail {

// Convert a Frequency to a Period for date arithmetic.
inline Period to_period(Frequency freq) noexcept {
    switch(freq) {
    case Frequency::Annual:    return Period{1, Frequency::Annual};
    case Frequency::SemiAnnual: return Period{1, Frequency::SemiAnnual};
    case Frequency::Quarterly: return Period{1, Frequency::Quarterly};
    case Frequency::Monthly:   return Period{1, Frequency::Monthly};
    case Frequency::Weekly:    return Period{1, Frequency::Weekly};
    case Frequency::Daily:     return Period{1, Frequency::Daily};
    default:                   return Period{0, Frequency::Once};
    }
}

// Step one period forward, applying business-day convention and optional
// end-of-month logic.
inline Date step_forward(Date ref, Period p, const Calendar& cal,
                         BusinessDayConvention bdc, bool eom) noexcept {
    Date d = ref + p;
    if(eom && cal.is_business_day(ref) && ref.is_end_of_month()) {
        d = d.end_of_month();
    }
    return cal.adjust(d, bdc);
}

// Step one period backward.
inline Date step_backward(Date ref, Period p, const Calendar& cal,
                          BusinessDayConvention bdc, bool eom) noexcept {
    Date d = ref - p;
    if(eom && cal.is_business_day(ref) && ref.is_end_of_month()) {
        d = d.end_of_month();
    }
    return cal.adjust(d, bdc);
}

// Find the nth occurrence of a weekday in a given month.
// weekday: 1=Mon..7=Sun (matches spp::quant::Weekday).
// n: 1=first, 2=second, ..., 5=fifth.
inline Date nth_weekday(i32 year, u32 month, Weekday target_wd, u32 n) noexcept {
    Date first = Date::from_ymd(year, month, 1);
    u32 first_wd = static_cast<u32>(first.day_of_week());
    // Days until the first target weekday.
    i32 offset = static_cast<i32>(static_cast<u32>(target_wd) - first_wd);
    if(offset < 0) offset += 7;
    // First occurrence.
    Date occ = first + offset;
    // Advance to the n-th occurrence.
    occ = occ + static_cast<i32>((n - 1) * 7);
    // Guard against crossing into the next month.
    if(occ.month() != month) {
        // Return the last occurrence in the month (n-1).
        occ = occ - 7;
    }
    return occ;
}

// Check whether `d` is an IMM date: 3rd Wednesday of Mar / Jun / Sep / Dec.
inline bool is_imm_date(Date d) noexcept {
    u32 m = d.month();
    if(m != 3 && m != 6 && m != 9 && m != 12) return false;
    if(d.day_of_week() != Weekday::Wednesday) return false;
    // It must be the third Wednesday.
    Date third = nth_weekday(d.year(), m, Weekday::Wednesday, 3);
    return d == third;
}

// Return the next IMM date on or after `d`.
inline Date next_imm(Date d) noexcept {
    u32 m = d.month();
    i32 y = d.year();

    // IMM months are 3, 6, 9, 12.
    constexpr u32 imm_months[] = {3, 6, 9, 12};

    for(u32 i = 0; i < 4; i++) {
        if(imm_months[i] >= m) {
            Date third = nth_weekday(y, imm_months[i], Weekday::Wednesday, 3);
            if(third >= d) return third;
        }
    }
    // Cross into next year.
    return nth_weekday(y + 1, 3, Weekday::Wednesday, 3);
}

// Return the previous IMM date on or before `d`.
inline Date prev_imm(Date d) noexcept {
    u32 m = d.month();
    i32 y = d.year();

    constexpr u32 imm_months[] = {12, 9, 6, 3};

    for(u32 i = 0; i < 4; i++) {
        if(imm_months[i] <= m) {
            Date third = nth_weekday(y, imm_months[i], Weekday::Wednesday, 3);
            if(third <= d) return third;
        }
    }
    // Cross into previous year.
    return nth_weekday(y - 1, 12, Weekday::Wednesday, 3);
}

// ---- generation routines ---------------------------------------------------

inline Vec<Date> generate_backward(Date effective, Date termination,
                                   Frequency freq, const Calendar& cal,
                                   BusinessDayConvention bdc,
                                   BusinessDayConvention term_bdc, bool eom,
                                   Opt<Date> first, Opt<Date> next_to_last) {
    Vec<Date> dates;
    Period p = to_period(freq);

    Date term_adj = cal.adjust(termination, term_bdc);
    dates.push(term_adj);

    Date seed = term_adj;

    // If next_to_last is set, use it as the penultimate date and continue
    // backward from it.
    if(next_to_last.ok()) {
        dates.push(*next_to_last);
        seed = *next_to_last;
    }

    for(;;) {
        Date prev = step_backward(seed, p, cal, bdc, eom);
        if(prev < effective) break;

        // If first_date is set and we just passed it, insert it and stop
        // generating intermediate dates.
        if(first.ok() && prev == *first) {
            dates.push(prev);
            break;
        }
        dates.push(prev);
        seed = prev;
    }

    // Effective date goes first.
    if(first.ok()) {
        dates.push(*first);
    } else {
        dates.push(effective);
    }

    // Reverse to chronological order.
    u64 n = dates.length();
    for(u64 i = 0; i < n / 2; i++) {
        spp::swap(dates[i], dates[n - 1 - i]);
    }

    return dates;
}

inline Vec<Date> generate_forward(Date effective, Date termination,
                                  Frequency freq, const Calendar& cal,
                                  BusinessDayConvention bdc,
                                  BusinessDayConvention term_bdc, bool eom,
                                  Opt<Date> first, Opt<Date> next_to_last) {
    Vec<Date> dates;
    Period p = to_period(freq);

    Date seed = effective;
    dates.push(seed);

    if(first.ok()) {
        dates.clear();
        dates.push(*first);
        seed = *first;
    }

    Date term_adj = cal.adjust(termination, term_bdc);

    for(;;) {
        Date next_d = step_forward(seed, p, cal, bdc, eom);
        if(next_d > term_adj) break;
        dates.push(next_d);
        seed = next_d;
    }

    dates.push(term_adj);
    return dates;
}

inline Vec<Date> generate_third_wednesday(Date effective, Date termination,
                                          const Calendar& cal,
                                          BusinessDayConvention bdc,
                                          BusinessDayConvention term_bdc) {
    Vec<Date> dates;

    // Start at the first IMM date on or after effective.
    Date current = next_imm(effective);
    Date term_adj = cal.adjust(termination, term_bdc);

    while(current <= term_adj) {
        dates.push(cal.adjust(current, bdc));
        // Step one quarter.
        current = current + Period{1, Frequency::Quarterly};
        current = next_imm(current);
    }

    return dates;
}

inline Vec<Date> generate_cds(Date effective, Date termination,
                              const Calendar& cal,
                              BusinessDayConvention bdc,
                              BusinessDayConvention term_bdc) {
    // CDS standard schedule: quarterly IMM dates.
    // The first date is the previous IMM date relative to effective
    // (the accrual start).  Intermediate dates are quarterly IMM dates.
    Vec<Date> dates;

    Date first_imm = prev_imm(effective);
    Date term_adj = cal.adjust(termination, term_bdc);

    // First accrual start.
    dates.push(cal.adjust(first_imm, bdc));

    Date current = first_imm;
    for(;;) {
        current = current + Period{1, Frequency::Quarterly};
        current = next_imm(current);
        if(current > term_adj) break;
        dates.push(cal.adjust(current, bdc));
    }

    dates.push(term_adj);
    return dates;
}

inline Vec<Date> generate_zero(Date effective, Date termination,
                               BusinessDayConvention term_bdc,
                               const Calendar& cal) {
    Vec<Date> dates;
    dates.push(effective);
    dates.push(cal.adjust(termination, term_bdc));
    return dates;
}

} // namespace detail

// =========================================================================
// Schedule::make / make_short_rate
// =========================================================================

inline Schedule Schedule::make(Date effective_date, Date termination_date,
                               Frequency frequency, const Calendar& calendar,
                               BusinessDayConvention bdc,
                               BusinessDayConvention termination_bdc,
                               DateGeneration rule, bool end_of_month,
                               Opt<Date> first_date,
                               Opt<Date> next_to_last_date) {
    Vec<Date> dates;

    switch(rule) {
    case DateGeneration::Backward:
        dates = detail::generate_backward(effective_date, termination_date,
                                          frequency, calendar, bdc,
                                          termination_bdc, end_of_month,
                                          first_date, next_to_last_date);
        break;
    case DateGeneration::Forward:
        dates = detail::generate_forward(effective_date, termination_date,
                                         frequency, calendar, bdc,
                                         termination_bdc, end_of_month,
                                         first_date, next_to_last_date);
        break;
    case DateGeneration::ThirdWednesday:
        dates = detail::generate_third_wednesday(
            effective_date, termination_date, calendar, bdc, termination_bdc);
        break;
    case DateGeneration::Zero:
        dates = detail::generate_zero(effective_date, termination_date,
                                      termination_bdc, calendar);
        break;
    case DateGeneration::CDS:
        dates = detail::generate_cds(effective_date, termination_date,
                                     calendar, bdc, termination_bdc);
        break;
    }

    return Schedule{spp::move(dates)};
}

inline Schedule Schedule::make_short_rate(Date effective_date,
                                          Date termination_date,
                                          Frequency frequency,
                                          const Calendar& calendar,
                                          BusinessDayConvention bdc) {
    Vec<Date> dates;
    Period p = detail::to_period(frequency);

    dates.push(effective_date);
    Date seed = effective_date;

    for(;;) {
        Date next_d = seed + p;
        next_d = calendar.adjust(next_d, bdc);
        if(next_d > termination_date) break;
        dates.push(next_d);
        seed = next_d;
    }

    // Always include termination_date.
    if(dates.back() != termination_date) {
        dates.push(termination_date);
    }

    return Schedule{spp::move(dates)};
}

} // namespace spp::quant

SPP_NAMED_ENUM(::spp::quant::DateGeneration, "DateGeneration", Backward,
               SPP_CASE(Backward), SPP_CASE(Forward), SPP_CASE(ThirdWednesday),
               SPP_CASE(Zero), SPP_CASE(CDS));

SPP_NAMED_RECORD(::spp::quant::Schedule, "Schedule", SPP_FIELD(dates_));
