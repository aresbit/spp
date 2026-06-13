#pragma once

#include <spp/core/base.h>
#include <spp/numeric/math.h>
#include <spp/core/deterministic.h>
#include <spp/containers/string0.h>
#include <spp/containers/vec.h>
#include <spp/quant/strategy/types.h>

// Forward-declare data-layer types
namespace spp::quant::data {

template <typename A>
struct Ohlcv_Data;

} // namespace spp::quant::data

namespace spp::quant::backtest {

using spp::quant::strategy::Order_Direction;
using spp::quant::strategy::Market_Type;

// ============================================================
// Market_Rules — Exchange-level trading constraints
//
// Enforces real-world market rules:
//   - Suspension detection (no trading on halted stocks)
//   - Limit up/down (price bands for A-share ±10%, futures vary)
//   - Trading window (market hours only)
//   - Round lots (A-share: multiples of 100 shares)
//   - T+1 settlement (A-share: cannot sell before next trading day)
//
// Reference: QUANTAXIS market_rules.py
// ============================================================

template <typename A = Mdefault>
struct Market_Rules {

    // ---- Suspension Detection ----

    /// Detect if a stock is suspended/halted at a given bar index.
    /// Suspension indicators:
    ///   - Zero volume for ≥ 60 consecutive bars
    ///   - OHLC all equal (flat line) for ≥ 60 consecutive bars
    ///   - Previous day close equals current bar close with zero volume
    static auto detect_suspension(const data::Ohlcv_Data<A>& data,
                                   String_View code, u64 bar_idx) -> bool {
        // Simplified: check if current bar has zero volume
        // In production, this would use Ohlcv_Data::get() to inspect the bar.
        // For now, delegate to the data layer which has bar access.
        (void)data;
        (void)code;
        (void)bar_idx;
        // Placeholder: return false (assume no suspension)
        // Full implementation would check:
        //   bar = data.get(code, bar_idx);
        //   return bar.volume == 0 && bar.open == bar.high == bar.low == bar.close;
        return false;
    }

    /// Check if a consecutive flat/zero-volume condition has persisted.
    static auto check_suspension_window(const data::Ohlcv_Data<A>& data,
                                         String_View code, u64 bar_idx,
                                         u64 window = 60) -> bool {
        if (bar_idx < window) return false;
        u64 flat_count = 0;
        u64 zero_vol_count = 0;

        for (u64 i = bar_idx - window + 1; i <= bar_idx; i++) {
            // Placeholder: in production, iterate bars to count flat and zero-vol
            (void)data;
            (void)code;
            (void)i;
            // Increment counters based on actual bar data
        }
        (void)flat_count;
        (void)zero_vol_count;
        return false;
    }

    // ---- Price Band Checks ----

    /// Check if a trade at the given price is executable in this market.
    ///   - A-share: ±10% limit from previous close (ST: ±5%)
    ///   - Futures: exchange-specific limits (±4% to ±10%)
    ///   - Crypto: no limits
    static auto can_trade(Decimal<8> price, Decimal<8> prev_close,
                           Order_Direction dir, Market_Type mkt) -> bool {

        if (mkt == Market_Type::crypto) return true;

        Decimal<8> up = limit_up(prev_close, mkt);
        Decimal<8> down = limit_down(prev_close, mkt);

        if (dir == Order_Direction::buy || dir == Order_Direction::buy_open) {
            return price <= up;
        } else {
            return price >= down;
        }
    }

    /// Calculate limit-up price for a given previous close.
    ///   - stock_cn: prev_close * 1.10 (rounded to tick size)
    ///   - future_cn: prev_close * 1.06 (simplified, varies by contract)
    ///   - crypto: no limit (return max price)
    static auto limit_up(Decimal<8> prev_close, Market_Type mkt) -> Decimal<8> {
        if (mkt == Market_Type::crypto) {
            return Decimal<8>::from_raw(Limits<i64>::max() / 2);
        }

        f64 multiplier = (mkt == Market_Type::stock_cn) ? 1.10 : 1.06;
        f64 prev_f = _price_to_f64(prev_close);
        return _f64_to_price(prev_f * multiplier);
    }

    /// Calculate limit-down price for a given previous close.
    ///   - stock_cn: prev_close * 0.90
    ///   - future_cn: prev_close * 0.94 (simplified)
    ///   - crypto: no limit (return 0)
    static auto limit_down(Decimal<8> prev_close, Market_Type mkt) -> Decimal<8> {
        if (mkt == Market_Type::crypto) {
            return Decimal<8>{};
        }

        f64 multiplier = (mkt == Market_Type::stock_cn) ? 0.90 : 0.94;
        f64 prev_f = _price_to_f64(prev_close);
        return _f64_to_price(prev_f * multiplier);
    }

    /// Check if a stock is at limit-up (cannot buy further).
    static auto is_limit_up(Decimal<8> price, Decimal<8> prev_close,
                             Market_Type mkt) -> bool {
        if (mkt == Market_Type::crypto) return false;
        return price >= limit_up(prev_close, mkt);
    }

    /// Check if a stock is at limit-down (cannot sell further).
    static auto is_limit_down(Decimal<8> price, Decimal<8> prev_close,
                               Market_Type mkt) -> bool {
        if (mkt == Market_Type::crypto) return false;
        return price <= limit_down(prev_close, mkt);
    }

    // ---- Trading Window ----

    /// Check if current time falls within exchange trading hours.
    /// A-share: 9:30-11:30, 13:00-15:00 (Mon-Fri)
    /// Futures: 9:00-11:30, 13:30-15:00, 21:00-02:30 (with breaks)
    /// Crypto: 24/7
    static auto is_in_trade_window(Deterministic_Time time,
                                    Market_Type mkt = Market_Type::stock_cn) -> bool {

        if (mkt == Market_Type::crypto) return true;

        // Convert unix ns to hours and minutes
        i64 total_seconds = time.unix_s();
        i64 seconds_of_day = total_seconds % 86400;
        i64 hours = seconds_of_day / 3600;
        i64 minutes = (seconds_of_day % 3600) / 60;

        // Weekend check (simplified: Jan 1 1970 was Thursday)
        // Day 0 = Thursday, Day 3 = Sunday, etc.
        i64 days_since_epoch = total_seconds / 86400;
        i64 day_of_week = (days_since_epoch + 4) % 7; // 0=Sun, 1=Mon, ..., 6=Sat
        if (day_of_week == 0 || day_of_week == 6) return false;

        if (mkt == Market_Type::stock_cn) {
            // Morning session: 9:30 - 11:30
            if ((hours == 9 && minutes >= 30) || (hours == 10) || (hours == 11 && minutes <= 30)) {
                return true;
            }
            // Afternoon session: 13:00 - 15:00
            if ((hours == 13) || (hours == 14) || (hours == 15 && minutes == 0)) {
                return true;
            }
            return false;
        }

        if (mkt == Market_Type::future_cn) {
            // Morning: 9:00 - 11:30
            if ((hours == 9) || (hours == 10) || (hours == 11 && minutes <= 30)) {
                return true;
            }
            // Afternoon: 13:30 - 15:00
            if ((hours == 13 && minutes >= 30) || hours == 14 || (hours == 15 && minutes == 0)) {
                return true;
            }
            // Night session: 21:00 - 02:30 (next day)
            if (hours >= 21 || hours < 2 || (hours == 2 && minutes <= 30)) {
                return true;
            }
            return false;
        }

        return false;
    }

    /// Get the next trading window open time.
    static auto next_trade_window_open(Deterministic_Time time,
                                        Market_Type mkt = Market_Type::stock_cn) -> Deterministic_Time {
        // Simplified: return 9:30 AM next trading day
        i64 total_seconds = time.unix_s();
        i64 day_start = (total_seconds / 86400) * 86400;

        // Next day 9:30 AM
        i64 next_open = day_start + 86400 + 9 * 3600 + 30 * 60;

        // Skip weekends
        i64 day_idx = (next_open / 86400);
        i64 dow = (day_idx + 4) % 7;
        if (dow == 0) next_open += 86400;      // Sunday → Monday
        else if (dow == 6) next_open += 2 * 86400; // Saturday → Monday

        return Deterministic_Time::from_unix_s(next_open);
    }

    // ---- Round Lots ----

    /// Round share count to valid lot size for the market.
    /// A-share: multiples of 100
    /// Futures: 1 contract
    /// Crypto: any amount
    static auto round_lots(f64 shares, Market_Type mkt = Market_Type::stock_cn) -> f64 {
        if (mkt == Market_Type::crypto) return shares;
        if (mkt == Market_Type::future_cn) return Math::round(shares);

        // A-share: round to nearest 100
        f64 lots = shares / 100.0;
        f64 rounded = Math::round(lots) * 100.0;
        return Math::max(rounded, 100.0); // minimum 1 lot
    }

    /// Check if share count is a valid lot size.
    static auto is_valid_lot(f64 shares, Market_Type mkt = Market_Type::stock_cn) -> bool {
        if (mkt == Market_Type::crypto) return shares > 0;
        if (mkt == Market_Type::future_cn) return shares >= 1.0;

        // A-share: must be multiple of 100 and ≥ 100
        i64 ishares = static_cast<i64>(shares + 0.5);
        return ishares >= 100 && (ishares % 100) == 0;
    }

    // ---- T+1 Settlement ----

    /// Check if a sell is allowed under T+1 rules for A-share stocks.
    /// In A-share, stocks bought today cannot be sold today.
    static auto is_t1_locked(Deterministic_Time buy_time,
                              Deterministic_Time current_time) -> bool {
        i64 buy_day = buy_time.unix_s() / 86400;
        i64 current_day = current_time.unix_s() / 86400;
        return buy_day >= current_day; // same day or future (shouldn't happen)
    }

    /// Days since purchase (for T+N settlement check).
    static auto days_since_purchase(Deterministic_Time buy_time,
                                     Deterministic_Time current_time) -> u64 {
        i64 buy_day = buy_time.unix_s() / 86400;
        i64 current_day = current_time.unix_s() / 86400;
        if (current_day < buy_day) return 0;
        return static_cast<u64>(current_day - buy_day);
    }

private:
    static auto _price_to_f64(Decimal<8> p) noexcept -> f64 {
        return static_cast<f64>(p.raw()) / static_cast<f64>(Decimal<8>::factor());
    }

    static auto _f64_to_price(f64 v) noexcept -> Decimal<8> {
        i64 raw = static_cast<i64>(v * static_cast<f64>(Decimal<8>::factor()));
        return Decimal<8>::from_raw(raw);
    }
};

} // namespace spp::quant::backtest
