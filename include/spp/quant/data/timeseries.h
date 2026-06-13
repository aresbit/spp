#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"      // for Date, Period, Frequency
#include "spp/quant/base/calendar.h"  // for Calendar, BusinessDayConvention

namespace spp::quant {

// =========================================================================
// TimeSeries<V, A> — time-series keyed by Date (i32 serial).
//
// Primary storage is a Map<Date, V, A> for O(1) point lookups.
// Sorted vectors are maintained lazily (dirty_ flag) for iteration and
// range queries.  Slice views returned by dates()/values() are invalidated
// after any mutation (standard view-invalidation semantics).
// =========================================================================
template <typename V, typename A = Mdefault>
struct TimeSeries {
    Map<Date, V, A> data_;

    // ---- construction --------------------------------------------------
    static TimeSeries empty() noexcept { return TimeSeries{}; }
    TimeSeries() noexcept = default;

    // ---- mutation ------------------------------------------------------
    void set(Date d, V value) noexcept {
        if(data_.contains(d)) data_.erase(d);
        data_.insert(spp::move(d), spp::move(value));
        dirty_ = true;
    }

    // ---- exact lookup --------------------------------------------------
    [[nodiscard]] Opt<Ref<V>> get(Date d) noexcept { return data_.try_get(d); }
    [[nodiscard]] Opt<Ref<const V>> get(Date d) const noexcept {
        return data_.try_get(d);
    }
    [[nodiscard]] Opt<V> operator[](Date d) const noexcept {
        auto r = data_.try_get(d);
        if(!r.ok()) return {};
        return Opt{**r};
    }

    // ---- nearest-neighbour lookup (const only — quant reads dominate) ---
    [[nodiscard]] Opt<Ref<const V>> latest_before(Date d) const noexcept;
    [[nodiscard]] Opt<Ref<const V>> earliest_after(Date d) const noexcept;

    // ---- range / iteration ----------------------------------------------
    [[nodiscard]] Slice<const Date> dates() const noexcept;
    [[nodiscard]] Slice<const V> values() const noexcept;

    [[nodiscard]] u64 size() const noexcept { return data_.length(); }
    [[nodiscard]] Date first_date() const noexcept;
    [[nodiscard]] Date last_date() const noexcept;

    [[nodiscard]] TimeSeries slice(Date start, Date end) const noexcept;

private:
    void ensure_sorted() const noexcept;

    mutable Vec<Date, A> sorted_dates_;
    mutable Vec<V, A> sorted_values_;
    mutable bool dirty_ = false;
};

// =========================================================================
// detail: binary search on sorted Date array
// =========================================================================
namespace detail {
inline u64 ts_lower_bound(Slice<const Date> dates, Date target) noexcept {
    u64 lo = 0, hi = dates.length();
    while(lo < hi) {
        u64 mid = lo + (hi - lo) / 2;
        if(dates[mid] < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
} // namespace detail

// =========================================================================
// Template member implementations
// =========================================================================

template <typename V, typename A>
void TimeSeries<V, A>::ensure_sorted() const noexcept {
    if(!dirty_) return;

    u64 n = data_.length();
    sorted_dates_.clear();
    sorted_values_.clear();
    if(n == 0) {
        dirty_ = false;
        return;
    }

    sorted_dates_.reserve(n);
    sorted_values_.reserve(n);

    // Collect all (date, value) pairs.
    Vec<Pair<Date, V>, A> pairs;
    pairs.reserve(n);
    for(const auto& kv : data_) {
        pairs.push(Pair<Date, V>{kv.first, kv.second});
    }

    // Insertion-sort by date.  Time-series are typically small (hundreds
    // to low thousands of points) so quadratic sort is acceptable.
    for(u64 i = 1; i < n; i++) {
        Pair<Date, V> key = spp::move(pairs[i]);
        i64 j = static_cast<i64>(i) - 1;
        while(j >= 0 && pairs[static_cast<u64>(j)].first > key.first) {
            pairs[static_cast<u64>(j + 1)] =
                spp::move(pairs[static_cast<u64>(j)]);
            j--;
        }
        pairs[static_cast<u64>(j + 1)] = spp::move(key);
    }

    // Extract sorted columns.
    for(u64 i = 0; i < n; i++) {
        sorted_dates_.push(spp::move(pairs[i].first));
        sorted_values_.push(spp::move(pairs[i].second));
    }

    dirty_ = false;
}

template <typename V, typename A>
Opt<Ref<const V>> TimeSeries<V, A>::latest_before(Date d) const noexcept {
    ensure_sorted();
    u64 n = sorted_dates_.length();
    if(n == 0) return {};
    u64 idx = detail::ts_lower_bound(sorted_dates_.slice(), d);
    // If exact match, step to that entry.
    // Otherwise lower_bound returns first entry > d; step back one.
    if(idx < n && sorted_dates_[idx] == d) {
        return data_.try_get(d);
    }
    if(idx == 0) return {}; // all dates are after d
    Date key = sorted_dates_[idx - 1];
    return data_.try_get(key);
}

template <typename V, typename A>
Opt<Ref<const V>> TimeSeries<V, A>::earliest_after(Date d) const noexcept {
    ensure_sorted();
    u64 n = sorted_dates_.length();
    if(n == 0) return {};
    u64 idx = detail::ts_lower_bound(sorted_dates_.slice(), d);
    if(idx >= n) return {}; // all dates are before d
    Date key = sorted_dates_[idx];
    return data_.try_get(key);
}

template <typename V, typename A>
Slice<const Date> TimeSeries<V, A>::dates() const noexcept {
    ensure_sorted();
    return sorted_dates_.slice();
}

template <typename V, typename A>
Slice<const V> TimeSeries<V, A>::values() const noexcept {
    ensure_sorted();
    return sorted_values_.slice();
}

template <typename V, typename A>
Date TimeSeries<V, A>::first_date() const noexcept {
    ensure_sorted();
    return sorted_dates_.empty() ? Date{} : sorted_dates_.front();
}

template <typename V, typename A>
Date TimeSeries<V, A>::last_date() const noexcept {
    ensure_sorted();
    return sorted_dates_.empty() ? Date{} : sorted_dates_.back();
}

template <typename V, typename A>
TimeSeries<V, A> TimeSeries<V, A>::slice(Date start, Date end) const
    noexcept {
    ensure_sorted();
    TimeSeries ret;
    u64 n = sorted_dates_.length();
    for(u64 i = 0; i < n; i++) {
        Date d = sorted_dates_[i];
        if(d >= start && d <= end) {
            ret.set(d, V{sorted_values_[i]});
        }
        if(d > end) break;
    }
    return ret;
}

// =========================================================================
// Free functions
// =========================================================================

// Linear interpolation between the two bracketing dates.
// When extrapolate is false, returns empty Opt when d is outside the
// series' date range.  V must be usable with f64 arithmetic (the common
// case is V = f64).
template <typename V>
Opt<V> interpolate_value(const TimeSeries<V>& ts, Date d,
                         bool extrapolate = false) noexcept {
    auto dates = ts.dates();
    auto values = ts.values();
    u64 n = dates.length();
    if(n == 0) return {};

    u64 idx = detail::ts_lower_bound(dates, d);

    // Exact match.
    if(idx < n && dates[idx] == d) return Opt{values[idx]};

    // Interior: d lies between two observed dates.
    if(idx > 0 && idx < n) {
        Date d0 = dates[idx - 1];
        Date d1 = dates[idx];
        V v0 = values[idx - 1];
        V v1 = values[idx];
        f64 t = static_cast<f64>(d - d0) / static_cast<f64>(d1 - d0);
        return Opt{static_cast<V>(static_cast<f64>(v0) +
                                  (static_cast<f64>(v1) - static_cast<f64>(v0)) * t)};
    }

    if(!extrapolate) return {};

    // Extrapolate left (d is before the first observation).
    if(idx == 0 && n >= 2) {
        Date d0 = dates[0];
        Date d1 = dates[1];
        V v0 = values[0];
        V v1 = values[1];
        f64 t = static_cast<f64>(d - d0) / static_cast<f64>(d1 - d0);
        return Opt{static_cast<V>(static_cast<f64>(v0) +
                                  (static_cast<f64>(v1) - static_cast<f64>(v0)) * t)};
    }

    // Extrapolate right (d is after the last observation).
    if(idx >= n && n >= 2) {
        Date d0 = dates[n - 2];
        Date d1 = dates[n - 1];
        V v0 = values[n - 2];
        V v1 = values[n - 1];
        f64 t = static_cast<f64>(d - d0) / static_cast<f64>(d1 - d0);
        return Opt{static_cast<V>(static_cast<f64>(v0) +
                                  (static_cast<f64>(v1) - static_cast<f64>(v0)) * t)};
    }

    return {};
}

// Apply function fn to a rolling window of width `window` ending at d.
// Returns 0.0 when no data falls within the window.
template <typename V>
f64 rolling_window(const TimeSeries<V>& ts, Date d, Period window,
                   f64 (*fn)(Slice<const V>)) noexcept {
    auto dates = ts.dates();
    auto values = ts.values();
    u64 n = dates.length();
    if(n == 0) return 0.0;

    // Approximate conversion from Period to days.
    // A full implementation would use Calendar for precise date math.
    i32 window_days = 0;
    switch(window.unit_) {
    case Frequency::Daily:     window_days = window.length_; break;
    case Frequency::Weekly:    window_days = window.length_ * 7; break;
    case Frequency::Monthly:   window_days = window.length_ * 30; break;
    case Frequency::Quarterly: window_days = window.length_ * 90; break;
    case Frequency::SemiAnnual: window_days = window.length_ * 180; break;
    case Frequency::Annual:    window_days = window.length_ * 365; break;
    default:                   window_days = window.length_ * 30; break;
    }
    Date start{d.serial_ - window_days};

    Vec<V> window_vals;
    window_vals.reserve(n);
    for(u64 i = 0; i < n; i++) {
        if(dates[i] >= start && dates[i] <= d) {
            window_vals.push(values[i]);
        }
        if(dates[i] > d) break;
    }
    if(window_vals.empty()) return 0.0;

    return fn(window_vals.slice());
}

} // namespace spp::quant
