#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/factor/rolling.h>
#include <spp/quant/factor/math_util.h>
#include <spp/quant/factor/alpha101.h>
#include <spp/quant/factor/chan_fractal.h>
#include <spp/quant/factor/fft_features.h>

namespace spp::quant::factor {

template <typename A = Mdefault>
struct Q_Transformer {
    u32 sequence_length;
    f64 decay;
    u32 hidden_dim;
    Vec<f64, A> hidden_state;
    u64 bar_count;

    Q_Transformer() noexcept : sequence_length(16), decay(0.9), hidden_dim(8), bar_count(0) {
        hidden_state = Vec<f64, A>::make(hidden_dim);
    }

    Q_Transformer(u32 seq_len, f64 decay_rate, u32 hdim) noexcept
        : sequence_length(seq_len), decay(decay_rate), hidden_dim(hdim), bar_count(0) {
        hidden_state = Vec<f64, A>::make(hdim);
    }

    auto update_hidden(const Vec<f64, A>& context) -> void {
        if (hidden_state.length() == 0)
            hidden_state = Vec<f64, A>::make(hidden_dim);
        for (u32 i = 0; i < hidden_dim; i++) {
            f64 ctx_val = (i < context.length()) ? context[i] : 0.0;
            f64 act = detail::tanh(ctx_val);
            hidden_state[i] = decay * hidden_state[i] + (1.0 - decay) * act;
        }
        bar_count++;
    }

    auto attention_weights(const Vec<Vec<f64, A>, A>& features, u64 current_idx) -> Vec<f64, A> {
        u64 n = features.length();
        if (n == 0) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        auto weights = Vec<f64, A>::make(n);
        f64 total = 0.0;
        for (u64 i = 0; i < n; i++) {
            i64 dist = static_cast<i64>(current_idx) - static_cast<i64>(i);
            if (dist < 0) dist = 0;
            f64 tw = Math::exp(-static_cast<f64>(dist) * (1.0 - decay));
            weights[i] = tw;
            total += tw;
        }
        if (total > 1e-12)
            for (u64 i = 0; i < n; i++) weights[i] /= total;
        return weights;
    }

    auto momentum_score(const data::Ohlcv_Data<A>& data, u64 bar_idx) -> f64 {
        u64 n = data.bars.length();
        if (bar_idx < sequence_length || bar_idx >= n) return 0.0;
        f64 recent = price_to_f64(data.bars[bar_idx].bar.close);
        f64 past = price_to_f64(data.bars[bar_idx - sequence_length].bar.close);
        if (Math::abs(past) < 1e-12) return 0.0;
        f64 raw_mom = (recent - past) / past;
        f64 vol = 0.0;
        u64 start = (bar_idx >= sequence_length) ? bar_idx - sequence_length : 0;
        for (u64 i = start + 1; i <= bar_idx; i++) {
            f64 prev = price_to_f64(data.bars[i - 1].bar.close);
            if (Math::abs(prev) < 1e-12) continue;
            f64 r = (price_to_f64(data.bars[i].bar.close) - prev) / prev;
            vol += r * r;
        }
        u64 cnt = bar_idx - start;
        vol = (cnt > 0) ? Math::sqrt(vol / static_cast<f64>(cnt)) : 1.0;
        return (vol > 1e-12) ? raw_mom / vol : raw_mom;
    }

    auto structure_score(const Chan_Features<A>&, u64) -> f64 { return 0.0; }

    auto fractal_score(const Vec<Chan_Fractal, A>& fractals, u64 bar_idx) -> f64 {
        f64 score = 0.0;
        for (u64 i = 0; i < fractals.length(); i++)
            if (fractals[i].index == bar_idx)
                score += (fractals[i].type == Fractal_Type::top) ? 1.0 : -1.0;
        return score * 0.5;
    }

    auto alpha_score(const Vec<f64, A>& alpha_physical, u64 bar_idx) -> f64 {
        return (bar_idx < alpha_physical.length()) ? alpha_physical[bar_idx] : 0.0;
    }

    auto fft_score(const Map<String<A>, Vec<f64, A>, A>& fft_features, u64 bar_idx) -> f64 {
        f64 score = 0.0;
        auto bopt = fft_features.try_get("fft_burst"_v);
        if (bopt.ok()) {
            const auto& burst = *bopt;
            if (bar_idx < burst.length()) score += burst[bar_idx] * 0.4;
        }
        auto ropt = fft_features.try_get("regime_shift"_v);
        if (ropt.ok()) {
            const auto& rs = *ropt;
            if (bar_idx < rs.length()) score -= rs[bar_idx] * 0.2;
        }
        return score;
    }

    auto hidden_readout() -> f64 {
        f64 out = 0.0;
        for (u32 i = 0; i < hidden_dim && i < hidden_state.length(); i++)
            out += hidden_state[i];
        return (hidden_dim > 0) ? out / static_cast<f64>(hidden_dim) : 0.0;
    }

    auto event_gate(const Vec<f64, A>& fft_burst, u64 bar_idx, u32 confirm_bars) -> f64 {
        if (bar_idx + confirm_bars >= fft_burst.length()) return 0.0;
        f64 bv = fft_burst[bar_idx];
        f64 confirm = 0.0;
        for (u32 i = 1; i <= confirm_bars && bar_idx + i < fft_burst.length(); i++)
            if (fft_burst[bar_idx + i] > 0) confirm += 1.0;
        f64 cr = (confirm_bars > 0) ? confirm / static_cast<f64>(confirm_bars) : 0.0;
        return (bv > 0.5) ? detail::tanh(bv) * cr : 0.0;
    }

    auto risk_penalty(f64 volatility, f64 volume_z) -> f64 {
        f64 pen = detail::tanh(volatility * 3.0) * 0.3;
        pen += detail::tanh(Math::abs(volume_z) * 0.5) * 0.2;
        return Math::clamp(pen, 0.0, 0.5);
    }

    auto compute_signal(const data::Ohlcv_Data<A>& data, u64 bar_idx, bool require_event)
        -> f64 {
        u64 n = data.bars.length();
        if (bar_idx >= n) return 0.0;

        f64 mom = momentum_score(data, bar_idx);

        auto chan = Chan_Features<A>{};
        auto fractals = chan.detect_fractals(data);
        f64 f_score_val = fractal_score(fractals, bar_idx);

        auto alpha_phys = Alpha101<A>::alpha_physical(data);
        f64 a_score = alpha_score(alpha_phys, bar_idx);

        auto fft_feats = Fft_Features<A>::extract_all(data);
        f64 ff_score = fft_score(fft_feats, bar_idx);

        f64 ev = 0.0;
        if (require_event) {
            auto bopt = fft_feats.try_get("fft_burst"_v);
            if (bopt.ok()) ev = event_gate(*bopt, bar_idx, 3);
            if (ev < 0.3) return 0.0;
        }

        f64 signal = mom * 0.25 + a_score * 0.25 + ff_score * 0.15
                     + hidden_readout() * 0.15;

        f64 vol = 0.0;
        u32 vw = 20;
        if (bar_idx >= vw) {
            for (u64 i = bar_idx - vw + 1; i <= bar_idx; i++) {
                f64 prev = price_to_f64(data.bars[i - 1].bar.close);
                f64 cur = price_to_f64(data.bars[i].bar.close);
                if (Math::abs(prev) > 1e-12) {
                    f64 r = (cur - prev) / prev;
                    vol += r * r;
                }
            }
            vol = Math::sqrt(vol / static_cast<f64>(vw));
        }

        f64 vol_mean = 0.0, vol_std = 0.0;
        if (bar_idx >= vw) {
            for (u64 i = bar_idx - vw + 1; i <= bar_idx; i++)
                vol_mean += data.bars[i].bar.volume;
            vol_mean /= static_cast<f64>(vw);
            for (u64 i = bar_idx - vw + 1; i <= bar_idx; i++)
                vol_std += (data.bars[i].bar.volume - vol_mean)
                           * (data.bars[i].bar.volume - vol_mean);
            vol_std = Math::sqrt(vol_std / static_cast<f64>(vw));
        }
        f64 vol_z = (vol_std > 1e-12)
                         ? (data.bars[bar_idx].bar.volume - vol_mean) / vol_std
                         : 0.0;

        f64 penalty = risk_penalty(vol, vol_z);
        signal = signal * (1.0 - penalty);
        if (require_event) signal *= ev;
        return Math::clamp(signal, -1.0, 1.0);
    }
};

} // namespace spp::quant::factor
