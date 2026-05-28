#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/strategy/strategy.h"
#include "spp/quant/math/statistics.h"
#include "spp/quant/math/distributions.h"

namespace spp::quant {

// =========================================================================
// Signal — a single alpha signal
// =========================================================================

struct Signal {
    String_View      name_             = ""_v;
    SignalDirection  direction_        = SignalDirection::Flat;
    f64              strength_         = 0.0;   ///< [-1, 1] normalized signal strength
    f64              expected_return_  = 0.0;   ///< expected return over horizon (bps)
    f64              volatility_       = 0.0;   ///< expected volatility
    Date             timestamp_        = Date{};

    // Performance tracking (updated externally by the alpha research loop)
    f64              ic_             = 0.0;     ///< information coefficient (correlation with fwd returns)
    f64              icir_           = 0.0;     ///< IC information ratio: mean(IC)/std(IC)
    u64              observations_   = 0;

    /// Signal quality: ICIR (higher = better), with a floor at 0
    [[nodiscard]] f64 quality() const noexcept {
        if (observations_ < 20) return 0.0;
        return icir_ > 0.0 ? icir_ : 0.0;
    }

    SPP_RECORD(Signal, SPP_FIELD(name_), SPP_FIELD(direction_), SPP_FIELD(strength_),
               SPP_FIELD(expected_return_), SPP_FIELD(volatility_), SPP_FIELD(timestamp_));
};

// =========================================================================
// SignalCombiner — combine multiple alpha signals into one
// =========================================================================

struct SignalCombiner {
    enum struct Method : u8 {
        EqualWeight = 0,
        IC_Weighted = 1,
        Vol_Scaled  = 2,
        Rank_Based  = 3
    };

    Method method_ = Method::EqualWeight;

    /// Combine multiple signals into a single aggregate signal.
    ///
    /// EqualWeight: average all strengths (naive combination).
    /// IC_Weighted: weight each signal by its IC (better signals dominate).
    /// Vol_Scaled: scale each signal to unit vol, then average (risk parity).
    /// Rank_Based: convert strengths to ranks, average ranks (robust).

    [[nodiscard]] Signal combine(Slice<const Signal> signals) const {
        u64 n = signals.length();
        if (n == 0) return Signal{};
        if (n == 1) return Signal{signals[0]};

        Signal result;
        result.timestamp_ = signals[0].timestamp_;

        switch (method_) {
        case Method::EqualWeight: {
            f64 total_strength = 0.0;
            f64 total_ret = 0.0;
            f64 total_vol = 0.0;
            for (u64 i = 0; i < n; i++) {
                total_strength += signals[i].strength_;
                total_ret += signals[i].expected_return_;
                total_vol += signals[i].volatility_;
            }
            result.strength_        = total_strength / static_cast<f64>(n);
            result.expected_return_ = total_ret / static_cast<f64>(n);
            result.volatility_      = total_vol / static_cast<f64>(n);
            break;
        }
        case Method::IC_Weighted: {
            f64 total_weight = 0.0;
            f64 weighted_strength = 0.0;
            f64 weighted_ret = 0.0;
            for (u64 i = 0; i < n; i++) {
                f64 w = Math::max(0.0, signals[i].icir_);
                total_weight += w;
                weighted_strength += w * signals[i].strength_;
                weighted_ret     += w * signals[i].expected_return_;
            }
            if (total_weight > 1e-15) {
                result.strength_        = weighted_strength / total_weight;
                result.expected_return_ = weighted_ret / total_weight;
            }
            break;
        }
        case Method::Vol_Scaled: {
            f64 total_inv_vol = 0.0;
            f64 weighted_strength = 0.0;
            for (u64 i = 0; i < n; i++) {
                f64 inv_vol = (signals[i].volatility_ > 1e-15)
                    ? 1.0 / signals[i].volatility_ : 1.0;
                total_inv_vol += inv_vol;
                weighted_strength += inv_vol * signals[i].strength_;
            }
            if (total_inv_vol > 1e-15) {
                result.strength_ = weighted_strength / total_inv_vol;
            }
            break;
        }
        case Method::Rank_Based: {
            // Assign ranks to strengths (1 = weakest, n = strongest)
            Vec<u64> ranks = Vec<u64>::make(n);
            for (u64 i = 0; i < n; i++) {
                u64 rank = 1;
                for (u64 j = 0; j < n; j++) {
                    if (signals[j].strength_ < signals[i].strength_) rank++;
                }
                ranks[i] = rank;
            }
            f64 rank_sum = 0.0;
            f64 weighted_strength = 0.0;
            for (u64 i = 0; i < n; i++) {
                f64 w = static_cast<f64>(ranks[i]);
                rank_sum += w;
                weighted_strength += w * signals[i].strength_;
            }
            if (rank_sum > 0.0) {
                result.strength_ = weighted_strength / rank_sum;
            }
            break;
        }
        }

        // Set direction
        if (result.strength_ > 0.01)
            result.direction_ = SignalDirection::Buy;
        else if (result.strength_ < -0.01)
            result.direction_ = SignalDirection::Sell;
        else
            result.direction_ = SignalDirection::Flat;

        return result;
    }

    // =====================================================================
    // IC Decay — measure how quickly a signal's predictive power decays
    // =====================================================================

    struct ICDecay {
        Vec<f64> lag_ics_;   ///< IC at lag 1, 2, 3, ...
        f64      half_life_; ///< lag at which IC drops to half of lag-1 IC
    };

    /// Compute IC decay from a history of signals and forward returns.
    /// signal_history[i] corresponds to forward_returns[i] (the return
    /// realized after the signal was observed).
    [[nodiscard]] static ICDecay compute_ic_decay(
        Slice<const Signal> signal_history,
        Slice<const f64> forward_returns)
    {
        ICDecay decay;
        u64 n = Math::min(signal_history.length(), forward_returns.length());
        if (n < 10) return decay;

        // Extract signal strengths
        Vec<f64> strengths = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++)
            strengths[i] = signal_history[i].strength_;

        // Compute IC at various lags
        u64 max_lag = Math::min(static_cast<u64>(20), n / 3);
        for (u64 lag = 1; lag <= max_lag; lag++) {
            Vec<f64> lagged_strengths = Vec<f64>::make(n - lag);
            Vec<f64> fwd_rets = Vec<f64>::make(n - lag);
            for (u64 i = 0; i < n - lag; i++) {
                lagged_strengths[i] = strengths[i];
                fwd_rets[i] = forward_returns[i + lag];
            }
            f64 ic = stat::correlation(lagged_strengths.slice(), fwd_rets.slice());
            decay.lag_ics_.push(ic);
        }

        // Compute half-life: find lag where IC <= 0.5 * IC(lag=1)
        if (decay.lag_ics_.length() > 0) {
            f64 ic1 = Math::abs(decay.lag_ics_[0]);
            if (ic1 > 1e-15) {
                f64 target = 0.5 * ic1;
                for (u64 lag = 0; lag < decay.lag_ics_.length(); lag++) {
                    if (Math::abs(decay.lag_ics_[lag]) <= target) {
                        decay.half_life_ = static_cast<f64>(lag + 1);
                        break;
                    }
                }
                if (decay.half_life_ == 0.0)
                    decay.half_life_ = static_cast<f64>(max_lag);
            }
        }

        return decay;
    }
};

// =========================================================================
// AlphaModel — convert signals to expected returns
// =========================================================================

struct AlphaModel {
    /// Convert a vector of signals to expected returns for n assets.
    /// If signals are per-asset, maps 1:1. If fewer signals than assets,
    /// fills with zero for unmatched assets.
    [[nodiscard]] Vec<f64> expected_returns(Slice<const Signal> signals,
                                             u64 num_assets) const {
        Vec<f64> rets = Vec<f64>::make(num_assets);
        for (u64 i = 0; i < num_assets; i++) {
            rets[i] = (i < signals.length()) ? signals[i].expected_return_ : 0.0;
        }
        return rets;
    }

    /// Apply exponential decay weighting to signal history.
    /// Returns weighted average expected return, where weight = exp(-age / tau)
    /// and tau = half_life_days / ln(2).
    [[nodiscard]] Vec<f64> apply_decay(Slice<const Signal> signals,
                                        f64 half_life_days) const {
        u64 n = signals.length();
        Vec<f64> rets = Vec<f64>::make(n);
        if (n == 0 || half_life_days <= 0.0) return rets;

        f64 tau = half_life_days / Math::log(2.0);

        // Weights decay as we move forward in time. The most recent signal
        // (at index n-1) gets weight = 1.0.
        for (u64 i = 0; i < n; i++) {
            // Age in days: approximate from index difference
            f64 age = static_cast<f64>(n - 1 - i);
            f64 weight = Math::exp(-age / tau);
            rets[i] = signals[i].expected_return_ * weight;
        }
        return rets;
    }

    /// Cross-sectional normalization: z-score across all assets.
    /// After normalization, the signals are zero-mean, unit-variance.
    void normalize_cross_sectional(Vec<f64>& sigs) const {
        u64 n = sigs.length();
        if (n < 2) return;

        f64 sum = 0.0;
        for (u64 i = 0; i < n; i++) sum += sigs[i];
        f64 mean = sum / static_cast<f64>(n);

        f64 sum_sq = 0.0;
        for (u64 i = 0; i < n; i++) {
            f64 diff = sigs[i] - mean;
            sum_sq += diff * diff;
        }
        f64 std = Math::sqrt(sum_sq / static_cast<f64>(n - 1));
        if (std < 1e-15) return;

        for (u64 i = 0; i < n; i++)
            sigs[i] = (sigs[i] - mean) / std;
    }
};

// =========================================================================
// RiskModel — position sizing and risk management for strategies
// =========================================================================

struct RiskModel {
    // =====================================================================
    // Kelly Criterion: optimal fraction of capital to risk on a bet.
    //
    // f* = (p * b - q) / b   (discrete form, for binary outcomes)
    //   or equivalently:
    // f* = mu / sigma^2       (continuous form, for normal returns)
    //
    // where:
    //   mu  = expected return (drift)
    //   sigma^2 = variance of returns
    //   risk_aversion >= 1.0 (1.0 = full Kelly, 2.0 = half Kelly, etc.)
    //
    // Full Kelly maximizes log-wealth but has high volatility.
    // Half-Kelly (risk_aversion = 2.0) is a common practical choice.
    // =====================================================================

    [[nodiscard]] f64 kelly_fraction(f64 expected_return,
                                      f64 variance,
                                      f64 risk_aversion = 1.0) const {
        if (variance < 1e-15 || risk_aversion < 1e-15) return 0.0;
        f64 f_star = expected_return / (variance * risk_aversion);
        // Cap at 100% of capital
        if (f_star > 1.0) f_star = 1.0;
        if (f_star < 0.0) f_star = 0.0;
        return f_star;
    }

    // =====================================================================
    // Volatility Targeting: scale position to hit a target volatility.
    //
    // target_position = signal * (target_vol / current_vol)
    //
    // Example: signal = 1.0 (go long), target_vol = 0.15 (15% annual),
    //          current_vol = 0.30 (30% annual)
    //          => target_position = 1.0 * (0.15 / 0.30) = 0.50 (half size)
    // =====================================================================

    [[nodiscard]] f64 volatility_scaled_position(f64 signal,
                                                  f64 target_vol,
                                                  f64 current_vol) const {
        if (current_vol < 1e-15) return 0.0;
        f64 scale = target_vol / current_vol;
        // Cap leverage at 5x
        if (scale > 5.0) scale = 5.0;
        return signal * scale;
    }

    // =====================================================================
    // CVaR-Constrained Position Sizing
    //
    // Under normal return assumption:
    //   CVaR_alpha = mu + sigma * phi(z_alpha) / (1 - alpha)
    //
    // where z_alpha = normal_icdf(alpha), phi = normal_pdf.
    //
    // Constrain: position * CVaR_alpha <= max_cvar
    // => max_position = max_cvar / CVaR_alpha
    // =====================================================================

    [[nodiscard]] f64 cvar_constrained_position(f64 expected_return,
                                                 f64 volatility,
                                                 f64 max_cvar,
                                                 f64 confidence = 0.95) const {
        if (volatility < 1e-15) return 0.0;

        // Standard normal CVaR for a unit position:
        // For a normal N(mu, sigma), CVaR_alpha = mu + sigma * phi(z_alpha) / (1 - alpha)
        f64 z = dist::normal_icdf(1.0 - confidence);  // z_alpha for tail
        f64 phi_z = dist::normal_pdf(z);
        f64 cvar_per_unit = expected_return + volatility * phi_z / (1.0 - confidence);

        if (cvar_per_unit <= 0.0) {
            // Positive CVaR means the position reduces risk; allow full size
            return 1.0;
        }

        f64 max_pos = max_cvar / cvar_per_unit;
        if (max_pos > 1.0) max_pos = 1.0;
        if (max_pos < 0.0) max_pos = 0.0;
        return max_pos;
    }
};

} // namespace spp::quant

SPP_NAMED_ENUM(::spp::quant::SignalCombiner::Method, "SignalCombinerMethod",
               EqualWeight, SPP_CASE(EqualWeight), SPP_CASE(IC_Weighted),
               SPP_CASE(Vol_Scaled), SPP_CASE(Rank_Based));
