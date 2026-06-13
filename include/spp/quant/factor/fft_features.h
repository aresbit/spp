#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/factor/rolling.h>
#include <spp/quant/factor/math_util.h>

namespace spp::quant::factor {

template <typename A = Mdefault>
struct Fft_Features {

    static auto compute_dft(const Vec<f64, A>& signal, u32 window) -> Vec<f64, A> {
        auto power = Vec<f64, A>::make(window / 2 + 1);
        f64 pi = 3.141592653589793;
        for (u32 k = 0; k <= window / 2; k++) {
            f64 re = 0.0, im = 0.0;
            for (u32 m = 0; m < window; m++) {
                f64 angle = -2.0 * pi * static_cast<f64>(k) * static_cast<f64>(m)
                            / static_cast<f64>(window);
                re += signal[m] * Math::cos(angle);
                im += signal[m] * Math::sin(angle);
            }
            power[k] = re * re + im * im;
        }
        return power;
    }

    static auto high_band_ratio(const data::Ohlcv_Data<A>& data, u32 window = 64) -> Vec<f64, A> {
        u64 n = data.bars.length();
        if (n < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 out_len = n - window + 1;
        auto result = Vec<f64, A>::make(out_len);
        f64 cutoff = static_cast<f64>(window / 2 + 1) * 0.4;
        for (u64 i = 0; i < out_len; i++) {
            auto segment = Vec<f64, A>::make(window);
            for (u32 j = 0; j < window; j++)
                segment[j] = price_to_f64(data.bars[i + j].bar.close);
            auto power = compute_dft(segment, window);
            f64 total = power[0];
            u32 half = window / 2 + 1;
            for (u32 k = 1; k < half; k++) total += power[k];
            f64 high_pwr = 0.0;
            for (u32 k = 1; k < half; k++)
                if (static_cast<f64>(k) > cutoff) high_pwr += power[k];
            result[i] = (total > 1e-12) ? high_pwr / total : 0.0;
        }
        return result;
    }

    static auto peak_frequency(const data::Ohlcv_Data<A>& data, u32 window = 64) -> Vec<f64, A> {
        u64 n = data.bars.length();
        if (n < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 out_len = n - window + 1;
        auto result = Vec<f64, A>::make(out_len);
        for (u64 i = 0; i < out_len; i++) {
            auto segment = Vec<f64, A>::make(window);
            f64 mean_val = 0.0;
            for (u32 j = 0; j < window; j++)
                mean_val += price_to_f64(data.bars[i + j].bar.close);
            mean_val /= static_cast<f64>(window);
            for (u32 j = 0; j < window; j++)
                segment[j] = price_to_f64(data.bars[i + j].bar.close) - mean_val;
            auto power = compute_dft(segment, window);
            u32 half = window / 2 + 1;
            u32 max_k = 1;
            f64 max_pwr = power[1];
            for (u32 k = 2; k < half; k++) {
                if (power[k] > max_pwr) { max_pwr = power[k]; max_k = k; }
            }
            result[i] = static_cast<f64>(max_k);
        }
        return result;
    }

    static auto spectral_entropy(const data::Ohlcv_Data<A>& data, u32 window = 64)
        -> Vec<f64, A> {
        u64 n = data.bars.length();
        if (n < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 out_len = n - window + 1;
        auto result = Vec<f64, A>::make(out_len);
        for (u64 i = 0; i < out_len; i++) {
            auto segment = Vec<f64, A>::make(window);
            f64 mean_val = 0.0;
            for (u32 j = 0; j < window; j++)
                mean_val += price_to_f64(data.bars[i + j].bar.close);
            mean_val /= static_cast<f64>(window);
            for (u32 j = 0; j < window; j++)
                segment[j] = price_to_f64(data.bars[i + j].bar.close) - mean_val;
            auto power = compute_dft(segment, window);
            f64 total = power[0];
            u32 half = window / 2 + 1;
            for (u32 k = 1; k < half; k++) total += power[k];
            f64 entropy = 0.0;
            f64 log_bins = detail::ln(static_cast<f64>(half));
            for (u32 k = 0; k < half; k++) {
                f64 p = (total > 1e-12) ? power[k] / total : 0.0;
                if (p > 1e-15) entropy -= p * detail::ln(p);
            }
            result[i] = (log_bins > 1e-12) ? entropy / log_bins : 0.0;
        }
        return result;
    }

    static auto fft_burst(const data::Ohlcv_Data<A>& data, u32 window = 64) -> Vec<f64, A> {
        auto hbr = high_band_ratio(data, window);
        if (hbr.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.bars.length();
        u64 offset = n - hbr.length();
        u64 hlen = hbr.length();
        auto range_r = Vec<f64, A>::make(hlen);
        for (u64 i = 0; i < hlen; i++) {
            f64 mx = price_to_f64(data.bars[offset + i].bar.close);
            f64 mn = mx;
            for (u32 j = 1; j < window && offset + i + j < n; j++) {
                f64 px = price_to_f64(data.bars[offset + i + j].bar.close);
                if (px > mx) mx = px;
                if (px < mn) mn = px;
            }
            range_r[i] = (mn > 1e-12) ? (mx - mn) / mn : 0.0;
        }
        auto z_hbr = Rolling<A>::zscore(hbr, 20);
        u64 zl = z_hbr.length();
        auto result = Vec<f64, A>::make(zl);
        u64 rr_off = hlen - zl;
        for (u64 i = 0; i < zl; i++)
            result[i] = z_hbr[i] * (1.0 + range_r[rr_off + i]);
        return result;
    }

    static auto regime_shift(const data::Ohlcv_Data<A>& data, u32 window = 64) -> Vec<f64, A> {
        auto pk = peak_frequency(data, window);
        auto ent = spectral_entropy(data, window);
        if (pk.length() < 2 || ent.length() < 2) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 pl = pk.length() - 1;
        u64 el = ent.length() - 1;
        auto pk_diff = Vec<f64, A>::make(pl);
        auto ent_diff = Vec<f64, A>::make(el);
        for (u64 i = 0; i < pl; i++) pk_diff[i] = pk[i + 1] - pk[i];
        for (u64 i = 0; i < el; i++) ent_diff[i] = ent[i + 1] - ent[i];
        auto z_pk = Rolling<A>::zscore(pk_diff, 20);
        auto z_ent = Rolling<A>::zscore(ent_diff, 20);
        u64 res_len = z_pk.length();
        auto result = Vec<f64, A>::make(res_len);
        for (u64 i = 0; i < res_len; i++)
            result[i] = Math::abs(z_pk[i]) + Math::abs(z_ent[i]);
        return result;
    }

    static auto extract_all(const data::Ohlcv_Data<A>& data, u32 window = 64)
        -> Map<String<A>, Vec<f64, A>, A> {
        Map<String<A>, Vec<f64, A>, A> features;
        auto hbr = high_band_ratio(data, window);
        features.insert("high_band_ratio"_v.template string<A>(), spp::move(hbr));
        auto pk = peak_frequency(data, window);
        features.insert("peak_frequency"_v.template string<A>(), spp::move(pk));
        auto ent = spectral_entropy(data, window);
        features.insert("spectral_entropy"_v.template string<A>(), spp::move(ent));
        auto burst = fft_burst(data, window);
        features.insert("fft_burst"_v.template string<A>(), spp::move(burst));
        auto rs = regime_shift(data, window);
        features.insert("regime_shift"_v.template string<A>(), spp::move(rs));
        return features;
    }
};

} // namespace spp::quant::factor
