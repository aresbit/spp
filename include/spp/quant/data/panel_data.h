#pragma once
#include <spp/core/base.h>
#include <spp/quant/data/types.h>

namespace spp::quant::data {

template <typename A = Mdefault>
struct Panel_Data {
    Map<Deterministic_Time, Map<String<A>, f64, A>, A> data;

    Panel_Data() noexcept = default;
    Panel_Data(Panel_Data&&) noexcept = default;
    Panel_Data& operator=(Panel_Data&&) noexcept = default;

    [[nodiscard]] u64 time_count() const noexcept {
        return data.length();
    }

    [[nodiscard]] bool empty() const noexcept {
        return data.length() == 0;
    }

    [[nodiscard]] Vec<Deterministic_Time, A> times() const noexcept {
        Vec<Deterministic_Time, A> out;
        for(const auto& entry : data) {
            out.push(Deterministic_Time::from_unix_ns(entry.first.unix_ns()));
        }
        return out;
    }

    [[nodiscard]] Vec<String<A>, A> symbols() const noexcept {
        Map<String<A>, u8, A> seen;
        for(const auto& time_entry : data) {
            for(const auto& sym_entry : time_entry.second) {
                if(!seen.contains(sym_entry.first.view())) {
                    seen.insert(sym_entry.first.clone(), 1);
                }
            }
        }
        Vec<String<A>, A> out;
        for(const auto& sym_entry : seen) {
            out.push(sym_entry.first.clone());
        }
        return out;
    }

    [[nodiscard]] f64 mean() const noexcept {
        f64 sum = 0.0;
        u64 count = 0;
        for(const auto& time_entry : data) {
            for(const auto& sym_entry : time_entry.second) {
                sum += sym_entry.second;
                count++;
            }
        }
        return count > 0 ? sum / static_cast<f64>(count) : 0.0;
    }

    [[nodiscard]] f64 std() const noexcept {
        f64 m = mean();
        f64 sum_sq = 0.0;
        u64 count = 0;
        for(const auto& time_entry : data) {
            for(const auto& sym_entry : time_entry.second) {
                f64 diff = sym_entry.second - m;
                sum_sq += diff * diff;
                count++;
            }
        }
        if(count <= 1) return 0.0;
        return __builtin_sqrt(sum_sq / static_cast<f64>(count));
    }

    [[nodiscard]] Panel_Data<A> zscore() const noexcept {
        Panel_Data<A> result;
        for(const auto& time_entry : data) {
            auto t = Deterministic_Time::from_unix_ns(time_entry.first.unix_ns());
            auto& inner = time_entry.second;

            u64 n = inner.length();
            if(n == 0) continue;

            f64 cs_sum = 0.0;
            for(const auto& se : inner) {
                cs_sum += se.second;
            }
            f64 cs_mean = cs_sum / static_cast<f64>(n);

            f64 cs_sum_sq = 0.0;
            for(const auto& se : inner) {
                f64 diff = se.second - cs_mean;
                cs_sum_sq += diff * diff;
            }
            f64 cs_std = n > 1 ? __builtin_sqrt(cs_sum_sq / static_cast<f64>(n)) : 1.0;
            if(cs_std < 1e-12) cs_std = 1.0;

            Map<String<A>, f64, A> zrow;
            for(const auto& se : inner) {
                f64 z = (se.second - cs_mean) / cs_std;
                zrow.insert(se.first.clone(), f64{z});
            }
            result.data.insert(spp::move(t), spp::move(zrow));
        }
        return result;
    }

    [[nodiscard]] Panel_Data<A> rank() const noexcept {
        Panel_Data<A> result;
        for(const auto& time_entry : data) {
            auto t = Deterministic_Time::from_unix_ns(time_entry.first.unix_ns());
            auto& inner = time_entry.second;

            u64 n = inner.length();
            if(n == 0) continue;

            Vec<Pair<String<A>, f64>, A> pairs;
            for(const auto& se : inner) {
                pairs.push(Pair<String<A>, f64>{se.first.clone(), se.second});
            }

            Map<String<A>, f64, A> rrow;
            for(u64 si = 0; si < pairs.length(); si++) {
                u64 rank_count = 0;
                for(u64 sj = 0; sj < pairs.length(); sj++) {
                    if(pairs[sj].second <= pairs[si].second) rank_count++;
                }
                f64 pct = n > 1
                    ? static_cast<f64>(rank_count - 1) / static_cast<f64>(n - 1)
                    : 0.5;
                rrow.insert(pairs[si].first.clone(), f64{pct});
            }
            result.data.insert(spp::move(t), spp::move(rrow));
        }
        return result;
    }

    [[nodiscard]] Map<String<A>, Map<String<A>, f64, A>, A> correlation() const noexcept {
        Map<String<A>, Map<String<A>, f64, A>, A> result;

        Map<String<A>, Vec<f64, A>, A> sym_series;
        Vec<String<A>, A> sym_list;

        for(const auto& time_entry : data) {
            for(const auto& sym_entry : time_entry.second) {
                auto opt = sym_series.try_get(sym_entry.first.view());
                if(opt.ok()) {
                    (**opt).push(sym_entry.second);
                } else {
                    Vec<f64, A> vs;
                    vs.push(sym_entry.second);
                    sym_series.insert(sym_entry.first.clone(), spp::move(vs));
                    sym_list.push(sym_entry.first.clone());
                }
            }
        }

        u64 n = sym_list.length();
        for(u64 i = 0; i < n; i++) {
            Map<String<A>, f64, A> row;
            // `try_get` returns Opt<Ref<V>> by value — the Opt temp dies at
            // the end of the statement, so bind to a Ref* / Vec& we own.
            auto opt_i = sym_series.try_get(sym_list[i].view());
            if(!opt_i.ok()) continue;
            auto& series_i = **opt_i;

            for(u64 j = 0; j < n; j++) {
                if(i == j) {
                    row.insert(sym_list[j].clone(), f64{1.0});
                    continue;
                }
                auto opt_j = sym_series.try_get(sym_list[j].view());
                if(!opt_j.ok()) continue;
                auto& series_j = **opt_j;

                u64 m = series_i.length() < series_j.length()
                    ? series_i.length() : series_j.length();
                if(m < 3) {
                    row.insert(sym_list[j].clone(), f64{0.0});
                    continue;
                }

                f64 sum_i = 0.0, sum_j = 0.0;
                for(u64 k = 0; k < m; k++) {
                    sum_i += series_i[k];
                    sum_j += series_j[k];
                }
                f64 mean_i = sum_i / static_cast<f64>(m);
                f64 mean_j = sum_j / static_cast<f64>(m);

                f64 cov = 0.0, var_i = 0.0, var_j = 0.0;
                for(u64 k = 0; k < m; k++) {
                    f64 di = series_i[k] - mean_i;
                    f64 dj = series_j[k] - mean_j;
                    cov += di * dj;
                    var_i += di * di;
                    var_j += dj * dj;
                }
                f64 denom = __builtin_sqrt(var_i * var_j);
                f64 corr = denom > 1e-12 ? cov / denom : 0.0;
                row.insert(sym_list[j].clone(), f64{corr});
            }
            result.insert(sym_list[i].clone(), spp::move(row));
        }
        return result;
    }

    [[nodiscard]] Map<String<A>, Map<String<A>, f64, A>, A> cov_matrix() const noexcept {
        Map<String<A>, Map<String<A>, f64, A>, A> result;

        Map<String<A>, Vec<f64, A>, A> sym_series;
        Vec<String<A>, A> sym_list;

        for(const auto& time_entry : data) {
            for(const auto& sym_entry : time_entry.second) {
                auto opt = sym_series.try_get(sym_entry.first.view());
                if(opt.ok()) {
                    (**opt).push(sym_entry.second);
                } else {
                    Vec<f64, A> vs;
                    vs.push(sym_entry.second);
                    sym_series.insert(sym_entry.first.clone(), spp::move(vs));
                    sym_list.push(sym_entry.first.clone());
                }
            }
        }

        u64 n = sym_list.length();
        for(u64 i = 0; i < n; i++) {
            Map<String<A>, f64, A> row;
            auto opt_i = sym_series.try_get(sym_list[i].view());
            if(!opt_i.ok()) continue;
            auto& series_i = **opt_i;

            for(u64 j = 0; j < n; j++) {
                auto opt_j = sym_series.try_get(sym_list[j].view());
                if(!opt_j.ok()) continue;
                auto& series_j = **opt_j;

                u64 m = series_i.length() < series_j.length()
                    ? series_i.length() : series_j.length();
                if(m < 2) {
                    row.insert(sym_list[j].clone(), f64{0.0});
                    continue;
                }

                f64 sum_i = 0.0, sum_j = 0.0;
                for(u64 k = 0; k < m; k++) {
                    sum_i += series_i[k];
                    sum_j += series_j[k];
                }
                f64 mean_i = sum_i / static_cast<f64>(m);
                f64 mean_j = sum_j / static_cast<f64>(m);

                f64 cov = 0.0;
                for(u64 k = 0; k < m; k++) {
                    cov += (series_i[k] - mean_i) * (series_j[k] - mean_j);
                }
                f64 cov_val = m > 1 ? cov / static_cast<f64>(m - 1) : 0.0;
                row.insert(sym_list[j].clone(), f64{cov_val});
            }
            result.insert(sym_list[i].clone(), spp::move(row));
        }
        return result;
    }
};

} // namespace spp::quant::data
