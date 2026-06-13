#pragma once

#include <spp/core/base.h>
#include <spp/numeric/math.h>
#include <spp/core/deterministic.h>
#include <spp/containers/string0.h>

namespace spp::quant::backtest {

// ============================================================
// Slippage_Model — Transaction cost from crossing the spread
//
// Models the cost of executing at a worse price than the signal price:
//   fixed:   execution_price = signal_price +/- value
//   percent: execution_price = signal_price * (1 +/- value)
//   impact:  execution_price = signal_price * (1 +/- value * (Q/V)^0.6)
//
// Reference: QUANTAXIS market_rules.py:92-127
// ============================================================

template <typename A = Mdefault>
struct Slippage_Model {

    /// Apply slippage to a signal price.
    ///   - price:         the signal/mid price
    ///   - slippage_value: slippage parameter
    ///   - model:         "fixed", "percent", or "impact"
    ///   - trade_size:    quantity being traded (Q)
    ///   - volume:        market volume at this bar (V)
    ///   - volatility:    recent volatility estimate (sigma)
    ///   - direction:     1 for buy, -1 for sell (default 1)
    static auto apply(Decimal<8> price, f64 slippage_value,
                      String_View model, f64 trade_size, f64 volume,
                      f64 volatility, i32 direction = 1) -> Decimal<8> {

        if (slippage_value <= 0.0 || volume <= 0.0) return price;

        f64 adj_factor = 0.0;
        f64 dir_sign = static_cast<f64>(direction);

        // Check model type by first character
        if (model.length() > 0 && model[0] == 'f') {
            // fixed: price +/- absolute value
            adj_factor = 1.0 + dir_sign * (slippage_value / _price_to_f64(price));
        }
        else if (model.length() > 0 && model[0] == 'i') {
            // impact: price * (1 +/- value * (trade_size/volume)^0.6)
            f64 participation = trade_size / volume;
            f64 impact = slippage_value * Math::pow(participation, 0.6);
            adj_factor = 1.0 + dir_sign * impact;
        }
        else {
            // percent (default): price * (1 +/- value)
            adj_factor = 1.0 + dir_sign * slippage_value;
        }

        return _f64_to_price(_price_to_f64(price) * adj_factor);
    }

    /// Apply a random jitter to simulate bid-ask bounce.
    /// Adds +/- volatility * N(0,1) component to the price.
    /// Uses a simple deterministic approximation (no RNG dependency).
    static auto apply_jitter(Decimal<8> price, f64 volatility) -> Decimal<8> {
        if (volatility <= 0.0) return price;

        // Deterministic jitter: based on price raw value as pseudo-random seed
        i64 raw = price.raw();
        f64 pseudo_rand = static_cast<f64>((raw * 1103515245 + 12345) & 0x7FFFFFFF)
                          / static_cast<f64>(0x7FFFFFFF);
        pseudo_rand = (pseudo_rand - 0.5) * 2.0; // [-1, 1]

        f64 price_f = _price_to_f64(price);
        f64 jittered = price_f * (1.0 + pseudo_rand * volatility);
        return _f64_to_price(jittered);
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

// ============================================================
// Impact_Model — Market impact from order size
//
// Models the adverse price movement caused by the trade itself:
//   square_root:    I = coeff * sigma * sqrt(Q/V)  (Almgren et al. 2005)
//   linear:         I = coeff * sigma * (Q / (V * T))
//   almgren_chriss: permanent I = gamma * sigma * sign(Q) * |Q/V|
//                   temporary I = eta * sigma * (Q / (V * T))
//
// Reference: Almgren & Chriss (2001), "Optimal Execution of Portfolio Transactions"
// ============================================================

template <typename A = Mdefault>
struct Impact_Model {

    /// Estimate market impact cost for a trade.
    ///   - model:       "square_root", "linear", "almgren_chriss", "none_"
    ///   - coeff:       impact coefficient (gamma for permanent, eta for temporary)
    ///   - sigma:       volatility estimate (daily std dev of returns)
    ///   - trade_size:  quantity being traded (Q)
    ///   - volume:      expected market volume over execution horizon (V)
    ///   - price:       current market price
    ///   - bar_count:   number of bars over which to execute (T)
    ///   - direction:   +1 buy, -1 sell
    static auto estimate(String_View model, f64 coeff, f64 sigma,
                          f64 trade_size, f64 volume, Decimal<8> price,
                          u32 bar_count, i32 direction = 1) -> Decimal<8> {

        if (coeff <= 0.0 || sigma <= 0.0 || volume <= 0.0 || bar_count == 0) {
            return price;
        }

        f64 price_f = _price_to_f64(price);
        f64 dir_sign = static_cast<f64>(direction);
        f64 impact = 0.0;

        if (model.length() >= 2 && model[0] == 's' && model[1] == 'q') {
            // square_root: I = coeff * sigma * sqrt(Q/V)
            f64 participation = trade_size / volume;
            if (participation > 1.0) participation = 1.0;
            impact = coeff * sigma * Math::sqrt(participation);
        }
        else if (model.length() > 0 && model[0] == 'l') {
            // linear: I = coeff * sigma * (Q / (V * T))
            f64 daily_volume = volume * static_cast<f64>(bar_count);
            f64 participation = trade_size / daily_volume;
            if (participation > 1.0) participation = 1.0;
            impact = coeff * sigma * participation;
        }
        else if (model.length() > 0 && model[0] == 'a') {
            // almgren_chriss: permanent + temporary
            f64 participation = trade_size / volume;
            if (participation > 1.0) participation = 1.0;

            // Permanent impact: gamma * sigma * (Q/V)
            f64 permanent = coeff * sigma * participation;

            // Temporary impact: eta * sigma * (Q / (V * T))^0.6
            f64 temp_participation = trade_size / (volume * static_cast<f64>(bar_count));
            if (temp_participation > 1.0) temp_participation = 1.0;
            f64 temporary = coeff * 0.5 * sigma * Math::pow(temp_participation, 0.6);

            impact = permanent + temporary;
        }
        else {
            // none_: no impact
            return price;
        }

        f64 impacted_price = price_f * (1.0 + dir_sign * impact);
        return _f64_to_price(impacted_price);
    }

    /// Estimate execution shortfall in basis points.
    static auto shortfall_bps(String_View model, f64 coeff, f64 sigma,
                               f64 trade_size, f64 volume, u32 bar_count) -> f64 {
        if (coeff <= 0.0 || sigma <= 0.0 || volume <= 0.0 || bar_count == 0) {
            return 0.0;
        }

        f64 participation = trade_size / volume;
        if (participation > 1.0) participation = 1.0;

        if (model.length() >= 2 && model[0] == 's' && model[1] == 'q') {
            return coeff * sigma * Math::sqrt(participation) * 10000.0;
        }
        else if (model.length() > 0 && model[0] == 'l') {
            f64 daily_volume = volume * static_cast<f64>(bar_count);
            f64 p = trade_size / daily_volume;
            if (p > 1.0) p = 1.0;
            return coeff * sigma * p * 10000.0;
        }
        else if (model.length() > 0 && model[0] == 'a') {
            f64 permanent = coeff * sigma * participation;
            f64 temp_p = trade_size / (volume * static_cast<f64>(bar_count));
            if (temp_p > 1.0) temp_p = 1.0;
            f64 temporary = coeff * 0.5 * sigma * Math::pow(temp_p, 0.6);
            return (permanent + temporary) * 10000.0;
        }
        return 0.0;
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
