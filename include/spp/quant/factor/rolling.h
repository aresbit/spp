#pragma once

#include <spp/core/base.h>

namespace spp::quant::factor {

template <typename A = Mdefault>
struct Rolling {

    static auto mean(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window == 0 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        f64 sum_val = 0.0;
        for (u32 i = 0; i < window; i++) sum_val += data[i];
        result[0] = sum_val / static_cast<f64>(window);
        for (u64 i = 1; i < n; i++) {
            sum_val += data[i + window - 1] - data[i - 1];
            result[i] = sum_val / static_cast<f64>(window);
        }
        return result;
    }

    static auto std(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window < 2 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 m = data[i];
            f64 s = 0.0;
            for (u32 j = 1; j < window; j++) {
                f64 x = data[i + j];
                f64 old_m = m;
                m = old_m + (x - old_m) / static_cast<f64>(j + 1);
                s = s + (x - old_m) * (x - m);
            }
            result[i] = Math::sqrt(s / static_cast<f64>(window - 1));
        }
        return result;
    }

    static auto zscore(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window < 2 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 m = data[i];
            f64 s = 0.0;
            for (u32 j = 1; j < window; j++) {
                f64 x = data[i + j];
                f64 old_m = m;
                m = old_m + (x - old_m) / static_cast<f64>(j + 1);
                s = s + (x - old_m) * (x - m);
            }
            f64 std_val = Math::sqrt(s / static_cast<f64>(window - 1));
            result[i] = (std_val < 1e-12) ? 0.0 : (data[i + window - 1] - m) / std_val;
        }
        return result;
    }

    static auto ema(const Vec<f64, A>& data, u32 period) -> Vec<f64, A> {
        if (period == 0 || data.length() == 0) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        f64 alpha = 2.0 / static_cast<f64>(period + 1);
        u64 n = data.length();
        auto result = Vec<f64, A>::make(n);
        result[0] = data[0];
        for (u64 i = 1; i < n; i++) {
            result[i] = alpha * data[i] + (1.0 - alpha) * result[i - 1];
        }
        return result;
    }

    static auto max(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window == 0 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 mx = data[i];
            for (u32 j = 1; j < window; j++) {
                if (data[i + j] > mx) mx = data[i + j];
            }
            result[i] = mx;
        }
        return result;
    }

    static auto min(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window == 0 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 mn = data[i];
            for (u32 j = 1; j < window; j++) {
                if (data[i + j] < mn) mn = data[i + j];
            }
            result[i] = mn;
        }
        return result;
    }

    static auto argmax(const Vec<f64, A>& data, u32 window) -> Vec<u64, A> {
        if (window == 0 || data.length() < window) {
            auto empty = Vec<u64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<u64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 mx = data[i];
            u64 idx = 0;
            for (u32 j = 1; j < window; j++) {
                if (data[i + j] > mx) {
                    mx = data[i + j];
                    idx = j;
                }
            }
            result[i] = idx;
        }
        return result;
    }

    static auto argmin(const Vec<f64, A>& data, u32 window) -> Vec<u64, A> {
        if (window == 0 || data.length() < window) {
            auto empty = Vec<u64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<u64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 mn = data[i];
            u64 idx = 0;
            for (u32 j = 1; j < window; j++) {
                if (data[i + j] < mn) {
                    mn = data[i + j];
                    idx = j;
                }
            }
            result[i] = idx;
        }
        return result;
    }

    static auto sum(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window == 0 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        f64 sum_val = 0.0;
        for (u32 i = 0; i < window; i++) sum_val += data[i];
        result[0] = sum_val;
        for (u64 i = 1; i < n; i++) {
            sum_val += data[i + window - 1] - data[i - 1];
            result[i] = sum_val;
        }
        return result;
    }

    static auto correlation(const Vec<f64, A>& x, const Vec<f64, A>& y, u32 window) -> Vec<f64, A> {
        if (window < 2 || x.length() < window || y.length() < window
            || x.length() != y.length()) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = x.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 mx = 0.0, my = 0.0;
            for (u32 j = 0; j < window; j++) {
                mx += x[i + j];
                my += y[i + j];
            }
            mx /= static_cast<f64>(window);
            my /= static_cast<f64>(window);

            f64 cov = 0.0, vx = 0.0, vy = 0.0;
            for (u32 j = 0; j < window; j++) {
                f64 dx = x[i + j] - mx;
                f64 dy = y[i + j] - my;
                cov += dx * dy;
                vx += dx * dx;
                vy += dy * dy;
            }
            f64 denom = Math::sqrt(vx * vy);
            result[i] = (denom < 1e-15) ? 0.0 : cov / denom;
        }
        return result;
    }

    static auto rank(const Vec<f64, A>& data, u32 window) -> Vec<f64, A> {
        if (window < 2 || data.length() < window) {
            auto empty = Vec<f64, A>{};
            empty.reserve(0);
            return empty;
        }
        u64 n = data.length() - window + 1;
        auto result = Vec<f64, A>::make(n);
        f64 win_f = static_cast<f64>(window);
        for (u64 i = 0; i < n; i++) {
            u64 less_count = 0;
            u64 eq_count = 0;
            f64 val = data[i + window - 1];
            for (u32 j = 0; j < window; j++) {
                f64 wv = data[i + j];
                if (wv < val) less_count++;
                else if (wv == val) eq_count++;
            }
            result[i] = (static_cast<f64>(less_count) + 0.5 * static_cast<f64>(eq_count)) / win_f;
        }
        return result;
    }
};

} // namespace spp::quant::factor
