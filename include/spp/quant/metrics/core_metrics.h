#pragma once

#include <spp/core/base.h>

namespace spp::quant::metrics {

struct Metrics_Report {
    f64 total_return = 0.0;
    f64 annual_return = 0.0;
    f64 annual_volatility = 0.0;
    f64 max_drawdown = 0.0;
    u32 max_drawdown_days = 0;
    f64 sharpe_ratio = 0.0;
    f64 sortino_ratio = 0.0;
    f64 calmar_ratio = 0.0;
    f64 var_95 = 0.0;
    f64 cvar_95 = 0.0;
    f64 skewness = 0.0;
    f64 kurtosis = 0.0;
    f64 win_rate = 0.0;
    f64 profit_factor = 0.0;
    u32 max_consecutive_losses = 0;
    f64 recovery_factor = 0.0;
    f64 rolling_sharpe_mean = 0.0;
    f64 rolling_sharpe_min = 0.0;
};

template <typename A = Mdefault>
struct Metrics_Calculator {

    static auto compute(const Vec<f64, A>& equity_curve, const Vec<f64, A>& returns,
                         u32 bars_per_year = 252) -> Metrics_Report;

    static auto sharpe(const Vec<f64, A>& returns, u32 bars_per_year) -> f64;
    static auto sortino(const Vec<f64, A>& returns, f64 target, u32 bars_per_year) -> f64;
    static auto calmar(f64 annual_return, f64 max_drawdown) -> f64;
    static auto max_drawdown(const Vec<f64, A>& equity) -> Pair<f64, u32>;
    static auto var(const Vec<f64, A>& returns, f64 confidence) -> f64;
    static auto cvar(const Vec<f64, A>& returns, f64 confidence) -> f64;
    static auto profit_factor(const Vec<f64, A>& returns) -> f64;
    static auto win_rate(const Vec<f64, A>& returns) -> f64;
    static auto rolling_sharpe(const Vec<f64, A>& returns, u32 window, u32 min_periods)
        -> Vec<f64, A>;
};

} // namespace spp::quant::metrics

SPP_NAMED_RECORD(spp::quant::metrics::Metrics_Report, "QM_Metrics_Report",
    SPP_FIELD(total_return), SPP_FIELD(annual_return), SPP_FIELD(annual_volatility),
    SPP_FIELD(max_drawdown), SPP_FIELD(max_drawdown_days), SPP_FIELD(sharpe_ratio),
    SPP_FIELD(sortino_ratio), SPP_FIELD(calmar_ratio), SPP_FIELD(var_95),
    SPP_FIELD(cvar_95), SPP_FIELD(skewness), SPP_FIELD(kurtosis), SPP_FIELD(win_rate),
    SPP_FIELD(profit_factor), SPP_FIELD(max_consecutive_losses),
    SPP_FIELD(recovery_factor), SPP_FIELD(rolling_sharpe_mean),
    SPP_FIELD(rolling_sharpe_min));

namespace spp::quant::metrics {

namespace detail {

inline auto sort_vec(Vec<f64, Mdefault>& v) -> void {
    u64 n = v.length();
    for (u64 i = 0; i < n; i++)
        for (u64 j = i + 1; j < n; j++)
            if (v[j] < v[i]) { f64 t = v[i]; v[i] = v[j]; v[j] = t; }
}

} // namespace detail

template <typename A>
auto Metrics_Calculator<A>::compute(const Vec<f64, A>& equity_curve,
                                      const Vec<f64, A>& returns,
                                      u32 bars_per_year) -> Metrics_Report {
    Metrics_Report report;
    u64 n = returns.length();
    if (n == 0) return report;

    f64 mean_ret = 0.0;
    for (u64 i = 0; i < n; i++) mean_ret += returns[i];
    mean_ret /= static_cast<f64>(n);

    f64 std_ret = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 d = returns[i] - mean_ret;
        std_ret += d * d;
    }
    std_ret = Math::sqrt(std_ret / static_cast<f64>(n));

    report.annual_return = mean_ret * static_cast<f64>(bars_per_year);
    report.annual_volatility
        = std_ret * Math::sqrt(static_cast<f64>(bars_per_year));

    if (equity_curve.length() > 0) {
        f64 start_eq = equity_curve[0];
        f64 end_eq = equity_curve[equity_curve.length() - 1];
        report.total_return = Math::abs(start_eq) > 1e-12
                                  ? (end_eq - start_eq) / Math::abs(start_eq)
                                  : 0.0;
    }

    auto dd = max_drawdown(equity_curve);
    report.max_drawdown = dd.first;
    report.max_drawdown_days = dd.second;

    report.sharpe_ratio = (std_ret > 1e-12)
                              ? mean_ret / std_ret
                                    * Math::sqrt(static_cast<f64>(bars_per_year))
                              : 0.0;

    report.sortino_ratio = sortino(returns, 0.0, bars_per_year);
    report.calmar_ratio
        = calmar(report.annual_return, report.max_drawdown);

    report.var_95 = var(returns, 0.95);
    report.cvar_95 = cvar(returns, 0.95);

    f64 m3 = 0.0, m4 = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 d = returns[i] - mean_ret;
        m3 += d * d * d;
        m4 += d * d * d * d;
    }
    m3 /= static_cast<f64>(n);
    m4 /= static_cast<f64>(n);
    f64 s3 = std_ret * std_ret * std_ret;
    f64 s4 = std_ret * std_ret * std_ret * std_ret;
    report.skewness = (s3 > 1e-15) ? m3 / s3 : 0.0;
    report.kurtosis = (s4 > 1e-15) ? m4 / s4 - 3.0 : 0.0;

    report.win_rate = win_rate(returns);
    report.profit_factor = profit_factor(returns);

    u32 consec = 0;
    report.max_consecutive_losses = 0;
    for (u64 i = 0; i < n; i++) {
        if (returns[i] < 0) {
            consec++;
            if (consec > report.max_consecutive_losses)
                report.max_consecutive_losses = consec;
        } else {
            consec = 0;
        }
    }

    report.recovery_factor = (report.max_drawdown > 1e-12)
                                 ? report.total_return / report.max_drawdown
                                 : 0.0;

    auto rs = rolling_sharpe(returns, 63, 30);
    if (rs.length() > 0) {
        f64 rs_mean = 0.0;
        f64 rs_min = rs[0];
        for (u64 i = 0; i < rs.length(); i++) {
            rs_mean += rs[i];
            if (rs[i] < rs_min) rs_min = rs[i];
        }
        report.rolling_sharpe_mean
            = rs_mean / static_cast<f64>(rs.length());
        report.rolling_sharpe_min = rs_min;
    }

    return report;
}

template <typename A>
auto Metrics_Calculator<A>::sharpe(const Vec<f64, A>& returns,
                                     u32 bars_per_year) -> f64 {
    u64 n = returns.length();
    if (n == 0) return 0.0;
    f64 m = 0.0;
    for (u64 i = 0; i < n; i++) m += returns[i];
    m /= static_cast<f64>(n);
    f64 s = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 d = returns[i] - m;
        s += d * d;
    }
    s = Math::sqrt(s / static_cast<f64>(n));
    return (s > 1e-12)
               ? m / s * Math::sqrt(static_cast<f64>(bars_per_year))
               : 0.0;
}

template <typename A>
auto Metrics_Calculator<A>::sortino(const Vec<f64, A>& returns, f64 target,
                                      u32 bars_per_year) -> f64 {
    u64 n = returns.length();
    if (n == 0) return 0.0;
    f64 m = 0.0;
    u64 dc = 0;
    f64 dv = 0.0;
    for (u64 i = 0; i < n; i++) {
        m += returns[i];
        f64 d = returns[i] - target;
        if (d < 0) { dv += d * d; dc++; }
    }
    m /= static_cast<f64>(n);
    if (dc == 0) return 0.0;
    f64 ds = Math::sqrt(dv / static_cast<f64>(dc));
    return (ds > 1e-12)
               ? (m - target) / ds * Math::sqrt(static_cast<f64>(bars_per_year))
               : 0.0;
}

template <typename A>
auto Metrics_Calculator<A>::calmar(f64 annual_return, f64 max_drawdown) -> f64 {
    return (Math::abs(max_drawdown) > 1e-12)
               ? annual_return / Math::abs(max_drawdown)
               : 0.0;
}

template <typename A>
auto Metrics_Calculator<A>::max_drawdown(const Vec<f64, A>& equity)
    -> Pair<f64, u32> {
    u64 n = equity.length();
    if (n == 0) return Pair<f64, u32>{0.0, 0};
    f64 peak = equity[0];
    u64 pi = 0;
    f64 max_dd = 0.0;
    u32 max_dd_days = 0;
    for (u64 i = 1; i < n; i++) {
        if (equity[i] > peak) { peak = equity[i]; pi = i; }
        if (Math::abs(peak) > 1e-12) {
            f64 dd = (peak - equity[i]) / peak;
            if (dd > max_dd) { max_dd = dd; max_dd_days = static_cast<u32>(i - pi); }
        }
    }
    return Pair<f64, u32>{max_dd, max_dd_days};
}

template <typename A>
auto Metrics_Calculator<A>::var(const Vec<f64, A>& returns, f64 confidence) -> f64 {
    u64 n = returns.length();
    if (n == 0) return 0.0;
    Vec<f64, Mdefault> sorted;
    sorted.reserve(n);
    for (u64 i = 0; i < n; i++) sorted.push(returns[i]);
    detail::sort_vec(sorted);
    u64 idx = static_cast<u64>(static_cast<f64>(n) * (1.0 - confidence));
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

template <typename A>
auto Metrics_Calculator<A>::cvar(const Vec<f64, A>& returns, f64 confidence) -> f64 {
    u64 n = returns.length();
    if (n == 0) return 0.0;
    Vec<f64, Mdefault> sorted;
    sorted.reserve(n);
    for (u64 i = 0; i < n; i++) sorted.push(returns[i]);
    detail::sort_vec(sorted);
    u64 cutoff = static_cast<u64>(static_cast<f64>(n) * (1.0 - confidence));
    if (cutoff > n) cutoff = n;
    if (cutoff == 0) return sorted[0];
    f64 sum_val = 0.0;
    for (u64 i = 0; i < cutoff; i++) sum_val += sorted[i];
    return sum_val / static_cast<f64>(cutoff);
}

template <typename A>
auto Metrics_Calculator<A>::profit_factor(const Vec<f64, A>& returns) -> f64 {
    f64 gp = 0.0, gl = 0.0;
    for (u64 i = 0; i < returns.length(); i++) {
        if (returns[i] > 0) gp += returns[i];
        else gl += Math::abs(returns[i]);
    }
    return (gl > 1e-12) ? gp / gl : 0.0;
}

template <typename A>
auto Metrics_Calculator<A>::win_rate(const Vec<f64, A>& returns) -> f64 {
    u64 wins = 0;
    u64 n = returns.length();
    for (u64 i = 0; i < n; i++)
        if (returns[i] > 0) wins++;
    return (n > 0) ? static_cast<f64>(wins) / static_cast<f64>(n) : 0.0;
}

template <typename A>
auto Metrics_Calculator<A>::rolling_sharpe(const Vec<f64, A>& returns, u32 window,
                                             u32 min_periods) -> Vec<f64, A> {
    u64 n = returns.length();
    if (n < min_periods || n < window) {
        auto empty = Vec<f64, A>{};
        empty.reserve(0);
        return empty;
    }
    u64 out_len = n - window + 1;
    auto result = Vec<f64, A>::make(out_len);
    f64 ann = Math::sqrt(252.0);
    for (u64 i = 0; i < out_len; i++) {
        f64 m = 0.0;
        for (u32 j = 0; j < window; j++) m += returns[i + j];
        m /= static_cast<f64>(window);
        f64 s = 0.0;
        for (u32 j = 0; j < window; j++) {
            f64 d = returns[i + j] - m;
            s += d * d;
        }
        s = Math::sqrt(s / static_cast<f64>(window));
        result[i] = (s > 1e-12) ? m / s * ann : 0.0;
    }
    return result;
}

} // namespace spp::quant::metrics
