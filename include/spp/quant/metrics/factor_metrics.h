#pragma once

#include <spp/core/base.h>
#include <spp/quant/data/panel_data.h>

namespace spp::quant::metrics {

struct Factor_IC_Report {
    f64 rank_ic_mean = 0.0;
    f64 rank_ic_std = 0.0;
    f64 rank_ic_ir = 0.0;
    Vec<f64, Mdefault> rank_ic_decay;
    f64 normal_ic_mean = 0.0;
    f64 normal_ic_std = 0.0;
    f64 normal_ic_ir = 0.0;
    Vec<f64, Mdefault> normal_ic_decay;
    f64 quintile_spread = 0.0;
    f64 factor_turnover = 0.0;
    f64 ic_pvalue = 0.0;
};

template <typename A = Mdefault>
struct Factor_Analyzer {
    static auto compute_ic(const data::Panel_Data<A>& factor_values,
                            const data::Panel_Data<A>& forward_returns)
        -> Factor_IC_Report;

    static auto icir_weighted_combine(const Vec<data::Panel_Data<A>, A>& factors,
                                       const data::Panel_Data<A>& returns)
        -> Vec<f64, A>;

    static auto quantile_analysis(const data::Panel_Data<A>& factor_values,
                                   const data::Panel_Data<A>& forward_returns,
                                   u32 n_quantiles) -> Vec<f64, A>;
};

} // namespace spp::quant::metrics

SPP_NAMED_RECORD(spp::quant::metrics::Factor_IC_Report, "QM_Factor_IC_Report",
    SPP_FIELD(rank_ic_mean), SPP_FIELD(rank_ic_std), SPP_FIELD(rank_ic_ir),
    SPP_FIELD(rank_ic_decay), SPP_FIELD(normal_ic_mean), SPP_FIELD(normal_ic_std),
    SPP_FIELD(normal_ic_ir), SPP_FIELD(normal_ic_decay), SPP_FIELD(quintile_spread),
    SPP_FIELD(factor_turnover), SPP_FIELD(ic_pvalue));

namespace spp::quant::metrics {

namespace detail {

inline auto sort_with_idx(Vec<f64, Mdefault>& v,
                                           Vec<u64, Mdefault>& idx) -> void {
    u64 n = v.length();
    for (u64 i = 0; i < n; i++)
        for (u64 j = i + 1; j < n; j++)
            if (v[j] < v[i]) {
                f64 tv = v[i]; v[i] = v[j]; v[j] = tv;
                u64 ti = idx[i]; idx[i] = idx[j]; idx[j] = ti;
            }
}

[[nodiscard]] inline auto rank_vec(const Vec<f64, Mdefault>& v) -> Vec<f64, Mdefault> {
    u64 n = v.length();
    if (n == 0) {
        auto empty = Vec<f64, Mdefault>{};
        empty.reserve(0);
        return empty;
    }
    Vec<u64, Mdefault> idx;
    Vec<f64, Mdefault> srt;
    idx.reserve(n);
    srt.reserve(n);
    for (u64 i = 0; i < n; i++) { srt.push(v[i]); idx.push(i); }
    sort_with_idx(srt, idx);
    auto ranks = Vec<f64, Mdefault>::make(n);
    u64 pos = 0;
    while (pos < n) {
        u64 start = pos;
        while (pos < n && srt[pos] == srt[start]) pos++;
        f64 ar = (static_cast<f64>(start) + static_cast<f64>(pos - 1)) / 2.0;
        for (u64 k = start; k < pos; k++)
            ranks[idx[k]] = ar / static_cast<f64>(n - 1);
    }
    return ranks;
}

[[nodiscard]] inline auto pearson_corr(const Vec<f64, Mdefault>& x,
                                         const Vec<f64, Mdefault>& y) -> f64 {
    u64 n = x.length();
    if (n < 2 || n != y.length()) return 0.0;
    f64 mx = 0.0, my = 0.0;
    for (u64 i = 0; i < n; i++) { mx += x[i]; my += y[i]; }
    mx /= static_cast<f64>(n);
    my /= static_cast<f64>(n);
    f64 cov = 0.0, vx = 0.0, vy = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 dx = x[i] - mx, dy = y[i] - my;
        cov += dx * dy; vx += dx * dx; vy += dy * dy;
    }
    f64 denom = Math::sqrt(vx * vy);
    return (denom > 1e-15) ? cov / denom : 0.0;
}

[[nodiscard]] inline auto spearman_corr(const Vec<f64, Mdefault>& x,
                                          const Vec<f64, Mdefault>& y) -> f64 {
    return pearson_corr(rank_vec(x), rank_vec(y));
}

} // namespace detail

template <typename A>
auto Factor_Analyzer<A>::compute_ic(const data::Panel_Data<A>& fv,
                                      const data::Panel_Data<A>& fr)
    -> Factor_IC_Report {
    Factor_IC_Report report;
    if (fv.data.length() == 0) return report;

    Vec<f64, Mdefault> rics, nics;
    rics.reserve(fv.data.length());
    nics.reserve(fv.data.length());

    for (const auto& te : fv.data) {
        auto ro = fr.data.try_get(te.first);
        if (!ro.ok()) continue;
        const auto& fm = te.second;
        const auto& rm = *ro;

        Vec<f64, Mdefault> x, y;
        for (const auto& se : fm) {
            auto re = rm.try_get(se.first.view());
            if (re.ok()) { x.push(se.second); y.push(*re); }
        }
        if (x.length() < 5) continue;
        rics.push(detail::spearman_corr(x, y));
        nics.push(detail::pearson_corr(x, y));
    }

    u64 nic = rics.length();
    if (nic > 0) {
        f64 s1 = 0.0, s2 = 0.0, n1 = 0.0, n2 = 0.0;
        for (u64 i = 0; i < nic; i++) {
            s1 += rics[i]; s2 += rics[i] * rics[i];
            n1 += nics[i]; n2 += nics[i] * nics[i];
        }
        report.rank_ic_mean = s1 / static_cast<f64>(nic);
        f64 rv = s2 / static_cast<f64>(nic)
                 - report.rank_ic_mean * report.rank_ic_mean;
        report.rank_ic_std = (rv > 0) ? Math::sqrt(rv) : 0.0;
        report.rank_ic_ir = (report.rank_ic_std > 1e-12)
                                ? report.rank_ic_mean / report.rank_ic_std
                                : 0.0;
        report.normal_ic_mean = n1 / static_cast<f64>(nic);
        f64 nv = n2 / static_cast<f64>(nic)
                 - report.normal_ic_mean * report.normal_ic_mean;
        report.normal_ic_std = (nv > 0) ? Math::sqrt(nv) : 0.0;
        report.normal_ic_ir = (report.normal_ic_std > 1e-12)
                                  ? report.normal_ic_mean / report.normal_ic_std
                                  : 0.0;
        if (report.rank_ic_std > 1e-12 && nic > 1) {
            f64 t = report.rank_ic_mean
                    / (report.rank_ic_std / Math::sqrt(static_cast<f64>(nic)));
            f64 at = Math::abs(t);
            if (at > 2.0) report.ic_pvalue = 0.01;
            else if (at > 1.645) report.ic_pvalue = 0.05;
            else report.ic_pvalue = 0.10;
        }
    }

    auto qr = quantile_analysis(fv, fr, 5);
    if (qr.length() >= 5)
        report.quintile_spread = qr[qr.length() - 1] - qr[0];

    return report;
}

template <typename A>
auto Factor_Analyzer<A>::icir_weighted_combine(
    const Vec<data::Panel_Data<A>, A>& factors, const data::Panel_Data<A>& returns)
    -> Vec<f64, A> {
    u64 nf = factors.length();
    auto w = Vec<f64, A>::make(nf);
    f64 total = 0.0;
    for (u64 i = 0; i < nf; i++) {
        auto rpt = compute_ic(factors[i], returns);
        f64 ir = Math::abs(rpt.rank_ic_ir);
        w[i] = ir;
        total += ir;
    }
    if (total > 1e-12)
        for (u64 i = 0; i < nf; i++) w[i] /= total;
    else
        for (u64 i = 0; i < nf; i++) w[i] = 1.0 / static_cast<f64>(nf);
    return w;
}

template <typename A>
auto Factor_Analyzer<A>::quantile_analysis(const data::Panel_Data<A>& fv,
                                             const data::Panel_Data<A>& fr,
                                             u32 nq) -> Vec<f64, A> {
    auto qr = Vec<f64, A>::make(nq);
    Vec<Vec<f64, Mdefault>, A> qb;
    for (u32 q = 0; q < nq; q++) qb.push(Vec<f64, Mdefault>{});

    for (const auto& te : fv.data) {
        auto ro = fr.data.try_get(te.first);
        if (!ro.ok()) continue;
        const auto& fm = te.second;
        const auto& rm = *ro;

        Vec<String<A>, A> syms;
        Vec<f64, Mdefault> scores;
        for (const auto& se : fm) {
            syms.push(se.first.template clone<A>());
            scores.push(se.second);
        }
        u64 ns = syms.length();
        if (ns < nq) continue;

        Vec<u64, Mdefault> idx;
        Vec<f64, Mdefault> srt;
        for (u64 i = 0; i < ns; i++) { srt.push(scores[i]); idx.push(i); }
        detail::sort_with_idx(srt, idx);

        u64 per_q = ns / static_cast<u64>(nq);
        for (u32 q = 0; q < nq; q++) {
            u64 start = q * per_q;
            u64 end = (q + 1 == nq) ? ns : (q + 1) * per_q;
            for (u64 s = start; s < end; s++) {
                auto rv = rm.try_get(syms[idx[s]].view());
                if (rv.ok()) qb[q].push(*rv);
            }
        }
    }

    for (u32 q = 0; q < nq; q++) {
        f64 sum_val = 0.0;
        u64 cnt = qb[q].length();
        for (u64 i = 0; i < cnt; i++) sum_val += qb[q][i];
        qr[q] = (cnt > 0) ? sum_val / static_cast<f64>(cnt) : 0.0;
    }
    return qr;
}

} // namespace spp::quant::metrics
