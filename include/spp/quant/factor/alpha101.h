#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/factor/rolling.h>
#include <spp/quant/factor/math_util.h>

namespace spp::quant::factor {

namespace detail {

template <typename A>
[[nodiscard]] static auto extract_close(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++)
        result[i] = price_to_f64(data.bars[i].bar.close);
    return result;
}
template <typename A>
[[nodiscard]] static auto extract_high(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++)
        result[i] = price_to_f64(data.bars[i].bar.high);
    return result;
}
template <typename A>
[[nodiscard]] static auto extract_low(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++)
        result[i] = price_to_f64(data.bars[i].bar.low);
    return result;
}
template <typename A>
[[nodiscard]] static auto extract_open(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++)
        result[i] = price_to_f64(data.bars[i].bar.open);
    return result;
}
template <typename A>
[[nodiscard]] static auto extract_volume(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++)
        result[i] = data.bars[i].bar.volume;
    return result;
}

template <typename A>
[[nodiscard]] static auto rank_vec(const Vec<f64, A>& v) -> Vec<f64, A> {
    u64 n = v.length();
    if (n == 0) {
        auto empty = Vec<f64, A>{};
        empty.reserve(0);
        return empty;
    }
    auto ranks = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) {
        u64 less = 0;
        u64 eq = 0;
        for (u64 j = 0; j < n; j++) {
            if (v[j] < v[i]) less++;
            else if (v[j] == v[i]) eq++;
        }
        ranks[i] = (static_cast<f64>(less) + 0.5 * static_cast<f64>(eq)) / static_cast<f64>(n);
    }
    return ranks;
}

template <typename A>
[[nodiscard]] static auto diff_vec(const Vec<f64, A>& v, u32 lag) -> Vec<f64, A> {
    u64 n = v.length();
    if (n <= lag) {
        auto empty = Vec<f64, A>{};
        empty.reserve(0);
        return empty;
    }
    auto result = Vec<f64, A>::make(n - lag);
    for (u64 i = 0; i < n - lag; i++)
        result[i] = v[i + lag] - v[i];
    return result;
}
template <typename A>
[[nodiscard]] static auto delay_vec(const Vec<f64, A>& v, u32 lag) -> Vec<f64, A> {
    u64 n = v.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < lag && i < n; i++) result[i] = 0.0;
    for (u64 i = lag; i < n; i++) result[i] = v[i - lag];
    return result;
}
template <typename A>
[[nodiscard]] static auto elementwise_multiply(const Vec<f64, A>& a, const Vec<f64, A>& b)
    -> Vec<f64, A> {
    u64 n = a.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = a[i] * b[i];
    return result;
}
template <typename A>
[[nodiscard]] static auto elementwise_subtract(const Vec<f64, A>& a, const Vec<f64, A>& b)
    -> Vec<f64, A> {
    u64 n = a.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = a[i] - b[i];
    return result;
}
template <typename A>
[[nodiscard]] static auto elementwise_add(const Vec<f64, A>& a, const Vec<f64, A>& b)
    -> Vec<f64, A> {
    u64 n = a.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = a[i] + b[i];
    return result;
}
template <typename A>
[[nodiscard]] static auto elementwise_scale(const Vec<f64, A>& v, f64 scale) -> Vec<f64, A> {
    u64 n = v.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = v[i] * scale;
    return result;
}
template <typename A>
[[nodiscard]] static auto ones_minus(const Vec<f64, A>& v) -> Vec<f64, A> {
    u64 n = v.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = 1.0 - v[i];
    return result;
}
template <typename A>
[[nodiscard]] static auto returns_vec(const Vec<f64, A>& prices) -> Vec<f64, A> {
    return diff_vec(prices, 1);
}
template <typename A>
[[nodiscard]] static auto ts_mean_vec(const Vec<f64, A>& v, u32 w) -> Vec<f64, A> {
    return Rolling<A>::mean(v, w);
}
template <typename A>
[[nodiscard]] static auto ts_std_vec(const Vec<f64, A>& v, u32 w) -> Vec<f64, A> {
    return Rolling<A>::std(v, w);
}
template <typename A>
[[nodiscard]] static auto ts_zscore_vec(const Vec<f64, A>& v, u32 w) -> Vec<f64, A> {
    return Rolling<A>::zscore(v, w);
}
template <typename A>
[[nodiscard]] static auto ts_max_vec(const Vec<f64, A>& v, u32 w) -> Vec<f64, A> {
    return Rolling<A>::max(v, w);
}
template <typename A>
[[nodiscard]] static auto ts_min_vec(const Vec<f64, A>& v, u32 w) -> Vec<f64, A> {
    return Rolling<A>::min(v, w);
}
template <typename A>
[[nodiscard]] static auto ts_rank_vec(const Vec<f64, A>& v, u32 w) -> Vec<f64, A> {
    return Rolling<A>::rank(v, w);
}
template <typename A>
[[nodiscard]] static auto ts_corr_vec(const Vec<f64, A>& x, const Vec<f64, A>& y, u32 w)
    -> Vec<f64, A> {
    return Rolling<A>::correlation(x, y, w);
}
template <typename A>
[[nodiscard]] static auto vec_abs(const Vec<f64, A>& v) -> Vec<f64, A> {
    u64 n = v.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = Math::abs(v[i]);
    return result;
}
template <typename A>
[[nodiscard]] static auto log_vec(const Vec<f64, A>& v) -> Vec<f64, A> {
    u64 n = v.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) result[i] = detail::ln(v[i] + 1e-12);
    return result;
}
template <typename A>
[[nodiscard]] static auto vwap(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
    u64 n = data.bars.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) {
        f64 h = price_to_f64(data.bars[i].bar.high);
        f64 l = price_to_f64(data.bars[i].bar.low);
        f64 c = price_to_f64(data.bars[i].bar.close);
        result[i] = (h + l + c) / 3.0;
    }
    return result;
}
template <typename A>
[[nodiscard]] static auto signed_power(const Vec<f64, A>& v, f64 p) -> Vec<f64, A> {
    u64 n = v.length();
    auto result = Vec<f64, A>::make(n);
    for (u64 i = 0; i < n; i++) {
        if (v[i] >= 0) result[i] = Math::pow(v[i], p);
        else result[i] = -Math::pow(-v[i], p);
    }
    return result;
}

} // namespace detail

template <typename A = Mdefault>
struct Alpha101 {

    static auto alpha_005(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto v = detail::extract_volume(data);
        auto vwap_ = detail::vwap(data);
        auto gap_close = detail::elementwise_subtract(vwap_, c);
        auto gap_rank = detail::rank_vec(gap_close);
        auto abs_gap = detail::vec_abs(gap_close);
        auto abs_gap_rank = detail::rank_vec(abs_gap);
        auto rank_vol = detail::rank_vec(v);
        auto adv20 = detail::ts_mean_vec(v, 20);
        u64 ln = c.length();
        auto vol_ratio = Vec<f64, A>::make(ln);
        for (u64 i = 0; i < ln; i++)
            vol_ratio[i] = (i < adv20.length() && adv20[i] > 0) ? v[i] / adv20[i] : 1.0;
        auto result = detail::elementwise_multiply(
            detail::ones_minus(gap_rank), detail::ones_minus(abs_gap_rank));
        result = detail::elementwise_multiply(result, rank_vol);
        for (u64 i = 0; i < result.length(); i++)
            result[i] = result[i] * vol_ratio[i];
        return detail::rank_vec(result);
    }

    static auto alpha_011(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto v = detail::extract_volume(data);
        auto vwap_ = detail::vwap(data);
        auto diff = detail::elementwise_subtract(c, vwap_);
        auto diff_rank = detail::rank_vec(diff);
        auto diff_max = detail::ts_max_vec(diff, 20);
        auto diff_min = detail::ts_min_vec(diff, 20);
        auto range = detail::elementwise_subtract(diff_max, diff_min);
        auto vol_delta = detail::diff_vec(v, 1);
        u64 n = c.length();
        auto vol_dir = Vec<f64, A>::make(n);
        vol_dir[0] = 0.0;
        for (u64 i = 1; i < n; i++)
            vol_dir[i] = (i - 1 < vol_delta.length() && vol_delta[i - 1] > 0) ? 1.0 : 0.0;
        auto vol_rank = detail::rank_vec(vol_dir);
        auto range_rank = detail::rank_vec(range);
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++)
            result[i] = diff_rank[i] * vol_rank[i] * range_rank[i];
        return detail::rank_vec(result);
    }

    static auto alpha_025(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto v = detail::extract_volume(data);
        u64 n = c.length();
        auto returns = detail::returns_vec(c);
        auto adv20 = detail::ts_mean_vec(v, 20);
        auto vol_ratio = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++)
            vol_ratio[i] = (i < adv20.length() && adv20[i] > 0) ? v[i] / adv20[i] : 1.0;
        auto rank_vol = detail::rank_vec(vol_ratio);
        auto rank_ret = detail::rank_vec(returns);
        auto result = detail::elementwise_multiply(rank_ret, rank_vol);
        auto signed_res = detail::signed_power(result, 0.5);
        return detail::rank_vec(signed_res);
    }

    static auto alpha_041(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto h = detail::extract_high(data);
        auto l = detail::extract_low(data);
        u64 n = c.length();
        auto vwap_ = detail::vwap(data);
        auto power = detail::elementwise_multiply(h, l);
        auto geom = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) geom[i] = Math::sqrt(power[i]);
        auto geom_rank = detail::rank_vec(geom);
        auto vwap_rank = detail::rank_vec(vwap_);
        auto diff_rank = detail::elementwise_subtract(geom_rank, vwap_rank);
        return detail::signed_power(diff_rank, 0.5);
    }

    static auto alpha_042(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto vwap_ = detail::vwap(data);
        auto price_avg = detail::ts_mean_vec(c, 5);
        auto vwap_avg = detail::ts_mean_vec(vwap_, 5);
        auto spread = detail::elementwise_subtract(price_avg, vwap_avg);
        u64 n = c.length();
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++)
            result[i] = (Math::abs(vwap_avg[i]) > 1e-12) ? spread[i] / vwap_avg[i] : 0.0;
        return detail::rank_vec(result);
    }

    static auto alpha_101(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto o = detail::extract_open(data);
        auto h = detail::extract_high(data);
        auto l = detail::extract_low(data);
        u64 n = c.length();
        auto raw = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 denom = h[i] - l[i];
            raw[i] = (denom > 1e-12) ? (c[i] - o[i]) / denom : 0.0;
        }
        auto r1 = detail::rank_vec(raw);
        auto r2 = detail::ts_mean_vec(r1, 3);
        return detail::rank_vec(r2);
    }

    static auto alpha_004(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto returns = detail::returns_vec(c);
        auto rank_ret = detail::rank_vec(returns);
        auto ts_rank_10 = detail::ts_rank_vec(c, 10);
        auto result = detail::elementwise_multiply(rank_ret, detail::ones_minus(ts_rank_10));
        return detail::rank_vec(result);
    }

    static auto alpha_007(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto v = detail::extract_volume(data);
        auto returns = detail::returns_vec(c);
        auto adv20 = detail::ts_mean_vec(v, 20);
        u64 n = c.length();
        auto abs_ret = detail::vec_abs(returns);
        auto adv_ratio = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++)
            adv_ratio[i] = (i < adv20.length() && adv20[i] > 0) ? v[i] / adv20[i] : 1.0;
        auto ts_max = detail::ts_max_vec(adv_ratio, 5);
        auto result = detail::elementwise_multiply(abs_ret, ts_max);
        auto rank = detail::rank_vec(result);
        return detail::elementwise_scale(rank, -1.0);
    }

    static auto alpha_012(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto v = detail::extract_volume(data);
        auto returns = detail::returns_vec(c);
        auto vol_delta = detail::diff_vec(v, 1);
        u64 n = c.length();
        auto sign_delta = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            if (i == 0 || i - 1 >= vol_delta.length()) {
                sign_delta[i] = 0.0;
            } else {
                sign_delta[i] = (vol_delta[i - 1] > 0) ? 1.0 : -1.0;
            }
        }
        return detail::rank_vec(detail::elementwise_multiply(sign_delta, returns));
    }

    static auto alpha_028(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto c = detail::extract_close(data);
        auto h = detail::extract_high(data);
        auto l = detail::extract_low(data);
        u64 n = c.length();
        auto returns = detail::returns_vec(c);
        auto rank_ret = detail::rank_vec(returns);
        auto hl_range = detail::elementwise_subtract(h, l);
        auto adv = detail::ts_mean_vec(c, 20);
        auto range_norm = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++)
            range_norm[i] = (i < adv.length() && adv[i] > 0) ? hl_range[i] / adv[i] : 0.0;
        auto corr = detail::ts_corr_vec(c, detail::log_vec(c), 10);
        auto result = detail::elementwise_multiply(rank_ret, range_norm);
        result = detail::elementwise_add(result, corr);
        return detail::rank_vec(result);
    }

    static auto alpha_physical(const data::Ohlcv_Data<A>& data) -> Vec<f64, A> {
        auto a05 = alpha_005(data);
        auto a11 = alpha_011(data);
        auto a25 = alpha_025(data);
        auto a41 = alpha_041(data);
        auto a42 = alpha_042(data);
        auto a101 = alpha_101(data);
        u64 n = a05.length();
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++)
            result[i] = (a05[i] + a11[i] + a25[i] + a41[i] + a42[i] + a101[i]) / 6.0;
        return detail::rank_vec(result);
    }
};

} // namespace spp::quant::factor
