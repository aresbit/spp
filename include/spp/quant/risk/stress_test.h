#pragma once

#include <spp/core/base.h>
#include <spp/core/rng.h>

namespace spp::quant::risk {

struct Scenario_Result {
    f64 total_return;
    f64 max_drawdown;
    f64 var_95;
    f64 cvar_95;
};

template <typename A = Mdefault>
struct Stress_Tester {

    static auto run_scenarios(const Vec<f64, A>& returns)
        -> Map<String<A>, Scenario_Result, A>;

    static auto monte_carlo_var(const Vec<f64, A>& returns, u32 horizon,
                                  u32 simulations, f64 confidence)
        -> Pair<f64, f64>;
};

namespace detail {

inline auto sort_vec(Vec<f64, Mdefault>& v) -> void {
    u64 n = v.length();
    for (u64 i = 0; i < n; i++)
        for (u64 j = i + 1; j < n; j++)
            if (v[j] < v[i]) { f64 t = v[i]; v[i] = v[j]; v[j] = t; }
}

[[nodiscard]] inline auto hist_var(const Vec<f64, Mdefault>& returns,
                                     f64 confidence) -> f64 {
    u64 n = returns.length();
    if (n == 0) return 0.0;
    Vec<f64, Mdefault> sorted;
    sorted.reserve(n);
    for (u64 i = 0; i < n; i++) sorted.push(returns[i]);
    sort_vec(sorted);
    u64 idx = static_cast<u64>(static_cast<f64>(n) * (1.0 - confidence));
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

[[nodiscard]] inline auto hist_cvar(const Vec<f64, Mdefault>& returns,
                                      f64 confidence) -> f64 {
    u64 n = returns.length();
    if (n == 0) return 0.0;
    Vec<f64, Mdefault> sorted;
    sorted.reserve(n);
    for (u64 i = 0; i < n; i++) sorted.push(returns[i]);
    sort_vec(sorted);
    u64 cutoff = static_cast<u64>(static_cast<f64>(n) * (1.0 - confidence));
    if (cutoff > n) cutoff = n;
    if (cutoff == 0) return sorted[0];
    f64 sum_val = 0.0;
    for (u64 i = 0; i < cutoff; i++) sum_val += sorted[i];
    return sum_val / static_cast<f64>(cutoff);
}

[[nodiscard]] inline auto max_dd_from_equity(const Vec<f64, Mdefault>& eq) -> f64 {
    u64 n = eq.length();
    if (n == 0) return 0.0;
    f64 peak = eq[0];
    f64 max_dd = 0.0;
    for (u64 i = 1; i < n; i++) {
        if (eq[i] > peak) peak = eq[i];
        if (Math::abs(peak) > 1e-12) {
            f64 dd = (peak - eq[i]) / peak;
            if (dd > max_dd) max_dd = dd;
        }
    }
    return max_dd;
}

} // namespace detail

template <typename A>
auto Stress_Tester<A>::run_scenarios(const Vec<f64, A>& returns)
    -> Map<String<A>, Scenario_Result, A> {
    Map<String<A>, Scenario_Result, A> results;
    u64 n = returns.length();
    if (n == 0) return results;

    f64 mean_ret = 0.0;
    for (u64 i = 0; i < n; i++) mean_ret += returns[i];
    mean_ret /= static_cast<f64>(n);

    f64 std_ret = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 d = returns[i] - mean_ret;
        std_ret += d * d;
    }
    std_ret = Math::sqrt(std_ret / static_cast<f64>(n));

    Scenario_Result fc;
    fc.total_return = -0.15;
    fc.max_drawdown = 0.20;
    fc.var_95 = detail::hist_var(returns, 0.95);
    fc.cvar_95 = detail::hist_cvar(returns, 0.95);
    results.insert("flash_crash_2010"_v.template string<A>(), fc);

    Scenario_Result cc;
    cc.total_return = -0.30;
    cc.max_drawdown = 0.35;
    cc.var_95 = detail::hist_var(returns, 0.99);
    cc.cvar_95 = detail::hist_cvar(returns, 0.99);
    results.insert("covid_crash_2020"_v.template string<A>(), cc);

    Vec<f64, Mdefault> shocked;
    shocked.reserve(n);
    for (u64 i = 0; i < n; i++) shocked.push(returns[i] - 4.0 * std_ret);
    Scenario_Result vs;
    vs.total_return = shocked.length() > 0 ? shocked[0] : 0.0;
    vs.max_drawdown = detail::max_dd_from_equity(shocked);
    vs.var_95 = detail::hist_var(shocked, 0.95);
    vs.cvar_95 = detail::hist_cvar(shocked, 0.95);
    results.insert("vol_spike_4sigma"_v.template string<A>(), vs);

    Scenario_Result rh;
    rh.total_return = -0.08;
    rh.max_drawdown = 0.12;
    rh.var_95 = detail::hist_var(returns, 0.95) * 1.5;
    rh.cvar_95 = detail::hist_cvar(returns, 0.95) * 1.5;
    results.insert("rate_hike_shock"_v.template string<A>(), rh);

    Vec<f64, Mdefault> cc2;
    cc2.reserve(n);
    for (u64 i = 0; i < n; i++) cc2.push(returns[i] * 1.8);
    Scenario_Result corr;
    corr.total_return = 0.0;
    for (u64 i = 0; i < cc2.length(); i++) corr.total_return += cc2[i];
    if (cc2.length() > 0) corr.total_return /= static_cast<f64>(cc2.length());
    corr.max_drawdown = detail::max_dd_from_equity(cc2);
    corr.var_95 = detail::hist_var(cc2, 0.95);
    corr.cvar_95 = detail::hist_cvar(cc2, 0.95);
    results.insert("correlation_crisis"_v.template string<A>(), corr);

    return results;
}

template <typename A>
auto Stress_Tester<A>::monte_carlo_var(const Vec<f64, A>& returns,
                                          u32 horizon, u32 sims, f64 confidence)
    -> Pair<f64, f64> {
    u64 n = returns.length();
    if (n == 0) return Pair<f64, f64>{0.0, 0.0};
    Vec<f64, Mdefault> sim_returns;
    sim_returns.reserve(sims);
    RNG::Stream rng{42};
    for (u32 s = 0; s < sims; s++) {
        f64 path_ret = 0.0;
        for (u32 h = 0; h < horizon; h++)
            path_ret += returns[rng.range<u64>(0, n)];
        sim_returns.push(path_ret);
    }
    detail::sort_vec(sim_returns);
    u32 ci = static_cast<u32>(static_cast<f64>(sims) * (1.0 - confidence));
    if (ci >= sims) ci = sims - 1;
    f64 var_val = sim_returns[ci];
    f64 cvar_sum = 0.0;
    for (u32 i = 0; i < ci; i++) cvar_sum += sim_returns[i];
    f64 cvar_val = (ci > 0) ? cvar_sum / static_cast<f64>(ci) : var_val;
    return Pair<f64, f64>{var_val, cvar_val};
}

} // namespace spp::quant::risk

SPP_NAMED_RECORD(spp::quant::risk::Scenario_Result, "QR_Scenario_Result",
    SPP_FIELD(total_return), SPP_FIELD(max_drawdown),
    SPP_FIELD(var_95), SPP_FIELD(cvar_95));
