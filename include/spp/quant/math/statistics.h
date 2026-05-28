#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <cmath>

#include "spp/quant/math/distributions.h"

namespace spp::quant::stat {

// ============================================================
// Univariate statistics
// ============================================================

struct UnivariateStats {
    f64 mean, variance, std_dev, skewness, kurtosis;
    f64 min, max, median;
};

// Compute comprehensive univariate statistics from a data slice.
// Uses numerically stable two-pass or Welford's algorithm.
[[nodiscard]] inline UnivariateStats compute_stats(Slice<const f64> data) noexcept {
    u64 n = data.length();
    if (n == 0) return {};

    // Welford's online algorithm for mean and variance in one pass
    f64 mean_val = 0.0;
    f64 m2 = 0.0;
    f64 min_val = data[0];
    f64 max_val = data[0];

    for (u64 i = 0; i < n; i++) {
        f64 x = data[i];
        f64 delta = x - mean_val;
        mean_val += delta / (f64)(i + 1);
        f64 delta2 = x - mean_val;
        m2 += delta * delta2;

        if (x < min_val) min_val = x;
        if (x > max_val) max_val = x;
    }

    f64 variance_val = (n > 1) ? m2 / (f64)(n - 1) : 0.0;
    f64 std_dev_val = Math::sqrt(variance_val);

    // Compute skewness and kurtosis in a second pass for accuracy
    f64 skew = 0.0;
    f64 kurt = 0.0;
    if (std_dev_val > 1e-15) {
        for (u64 i = 0; i < n; i++) {
            f64 z = (data[i] - mean_val) / std_dev_val;
            f64 z2 = z * z;
            skew += z2 * z;
            kurt += z2 * z2;
        }
        skew = skew / (f64)n;
        kurt = kurt / (f64)n - 3.0;  // excess kurtosis
    }

    // Compute median
    // Make a copy for sorting
    Vec<f64> sorted = Vec<f64>::make(n);
    for (u64 i = 0; i < n; i++) {
        sorted[i] = data[i];
    }
    // Simple insertion sort for small n; for larger n, this is acceptable in non-hot-path code
    for (u64 i = 1; i < n; i++) {
        f64 key = sorted[i];
        u64 j = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    f64 median_val;
    if (n % 2 == 1) {
        median_val = sorted[n / 2];
    } else {
        median_val = 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
    }

    return {mean_val, variance_val, std_dev_val, skew, kurt, min_val, max_val, median_val};
}

// ============================================================
// Covariance and correlation (univariate)
// ============================================================

[[nodiscard]] inline f64 covariance(Slice<const f64> x, Slice<const f64> y) noexcept {
    u64 n = Math::min(x.length(), y.length());
    if (n < 2) return 0.0;

    // Two-pass algorithm for numerical stability
    f64 mean_x = 0.0, mean_y = 0.0;
    for (u64 i = 0; i < n; i++) {
        mean_x += x[i];
        mean_y += y[i];
    }
    mean_x /= (f64)n;
    mean_y /= (f64)n;

    f64 cov = 0.0;
    for (u64 i = 0; i < n; i++) {
        cov += (x[i] - mean_x) * (y[i] - mean_y);
    }
    return cov / (f64)(n - 1);
}

[[nodiscard]] inline f64 correlation(Slice<const f64> x, Slice<const f64> y) noexcept {
    u64 n = Math::min(x.length(), y.length());
    if (n < 2) return 0.0;

    f64 mean_x = 0.0, mean_y = 0.0;
    for (u64 i = 0; i < n; i++) {
        mean_x += x[i];
        mean_y += y[i];
    }
    mean_x /= (f64)n;
    mean_y /= (f64)n;

    f64 cov = 0.0, var_x = 0.0, var_y = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 dx = x[i] - mean_x;
        f64 dy = y[i] - mean_y;
        cov += dx * dy;
        var_x += dx * dx;
        var_y += dy * dy;
    }

    if (var_x * var_y < 1e-30) return 0.0;
    return cov / Math::sqrt(var_x * var_y);
}

// ============================================================
// Covariance / correlation matrix
// ============================================================
// data: rows = observations, cols = variables

[[nodiscard]] inline Vec<f64> covariance_matrix(const Vec<Vec<f64>>& data) noexcept {
    u64 n_obs = data.length();
    u64 n_vars = n_obs > 0 ? data[0].length() : 0;
    if (n_obs < 2 || n_vars == 0) return Vec<f64>();

    // Check consistent dimensions
    for (u64 i = 1; i < n_obs; i++) {
        if (data[i].length() != n_vars) return Vec<f64>();
    }

    // Compute column means
    Vec<f64> means = Vec<f64>::make(n_vars);
    for (u64 j = 0; j < n_vars; j++) {
        f64 sum = 0.0;
        for (u64 i = 0; i < n_obs; i++) {
            sum += data[i][j];
        }
        means[j] = sum / (f64)n_obs;
    }

    // Compute covariance matrix (N x N, row-major)
    Vec<f64> result = Vec<f64>::make(n_vars * n_vars);
    for (u64 j = 0; j < n_vars; j++) {
        for (u64 k = 0; k < n_vars; k++) {
            f64 cov = 0.0;
            for (u64 i = 0; i < n_obs; i++) {
                cov += (data[i][j] - means[j]) * (data[i][k] - means[k]);
            }
            result[j * n_vars + k] = cov / (f64)(n_obs - 1);
        }
    }
    return result;
}

[[nodiscard]] inline Vec<f64> correlation_matrix(const Vec<Vec<f64>>& data) noexcept {
    Vec<f64> cov_mat = covariance_matrix(data);
    u64 n_vars = data.length() > 0 ? data[0].length() : 0;
    u64 n_total = cov_mat.length();

    if (n_total == 0 || n_vars == 0) return Vec<f64>();

    // Extract diagonal standard deviations
    Vec<f64> stds = Vec<f64>::make(n_vars);
    for (u64 j = 0; j < n_vars; j++) {
        stds[j] = Math::sqrt(cov_mat[j * n_vars + j]);
    }

    // Normalize cov to correlation
    Vec<f64> result = Vec<f64>::make(n_total);
    for (u64 j = 0; j < n_vars; j++) {
        for (u64 k = 0; k < n_vars; k++) {
            f64 denom = stds[j] * stds[k];
            if (denom < 1e-30) {
                result[j * n_vars + k] = (j == k) ? 1.0 : 0.0;
            } else {
                result[j * n_vars + k] = cov_mat[j * n_vars + k] / denom;
            }
        }
    }
    return result;
}

// ============================================================
// Ordinary Least Squares regression: y = alpha + beta * x
// ============================================================

struct OLS_Result {
    f64 alpha, beta, r_squared, std_error;
};

[[nodiscard]] inline OLS_Result ols_regression(Slice<const f64> x, Slice<const f64> y) noexcept {
    u64 n = Math::min(x.length(), y.length());
    if (n < 2) return {};

    f64 mean_x = 0.0, mean_y = 0.0;
    for (u64 i = 0; i < n; i++) {
        mean_x += x[i];
        mean_y += y[i];
    }
    mean_x /= (f64)n;
    mean_y /= (f64)n;

    f64 ss_xx = 0.0, ss_xy = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 dx = x[i] - mean_x;
        f64 dy = y[i] - mean_y;
        ss_xx += dx * dx;
        ss_xy += dx * dy;
    }

    f64 beta_val = 0.0;
    if (ss_xx > 1e-30) {
        beta_val = ss_xy / ss_xx;
    }
    f64 alpha_val = mean_y - beta_val * mean_x;

    // R-squared: SSR / SST
    f64 ss_res = 0.0, ss_tot = 0.0;
    for (u64 i = 0; i < n; i++) {
        f64 y_pred = alpha_val + beta_val * x[i];
        ss_res += (y[i] - y_pred) * (y[i] - y_pred);
        ss_tot += (y[i] - mean_y) * (y[i] - mean_y);
    }
    f64 r2 = (ss_tot > 1e-30) ? 1.0 - ss_res / ss_tot : 0.0;

    // Standard error of the regression
    f64 se = (n > 2) ? Math::sqrt(ss_res / (f64)(n - 2)) : 0.0;

    return {alpha_val, beta_val, r2, se};
}

// ============================================================
// PCA via covariance matrix eigendecomposition
// ============================================================
// For small matrices (max_components <= 50), uses power iteration
// with deflation as a simple and robust method.

struct PCA_Result {
    Vec<f64> eigenvalues;
    Vec<Vec<f64>> eigenvectors;  // eigenvectors[i] = i-th principal component
};

namespace detail {

struct PowerIterResult {
    f64 eigenvalue;
    Vec<f64> eigenvector;
};

// Power iteration to find the dominant eigenpair of a symmetric matrix (NxN, row-major)
// Returns (eigenvalue, eigenvector of length N)
[[nodiscard]] inline PowerIterResult power_iteration(Slice<const f64> mat, u64 N, u64 max_iter = 100) {
    Vec<f64> v = Vec<f64>::make(N);
    // Initialize with random-like deterministic vector
    for (u64 i = 0; i < N; i++) {
        v[i] = 1.0 / Math::sqrt((f64)(i + 1));
    }

    // Normalize initial vector
    f64 norm_v = 0.0;
    for (u64 i = 0; i < N; i++) norm_v += v[i] * v[i];
    norm_v = Math::sqrt(norm_v);
    if (norm_v > 1e-15) {
        for (u64 i = 0; i < N; i++) v[i] /= norm_v;
    }

    f64 lambda = 0.0;
    Vec<f64> w = Vec<f64>::make(N);

    for (u64 iter = 0; iter < max_iter; iter++) {
        // w = A * v
        for (u64 i = 0; i < N; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < N; j++) {
                sum += mat[i * N + j] * v[j];
            }
            w[i] = sum;
        }

        // lambda = v' * w (Rayleigh quotient)
        f64 new_lambda = 0.0;
        for (u64 i = 0; i < N; i++) {
            new_lambda += v[i] * w[i];
        }

        // Compute norm of w
        f64 norm_w = 0.0;
        for (u64 i = 0; i < N; i++) norm_w += w[i] * w[i];
        norm_w = Math::sqrt(norm_w);

        // Normalize
        if (norm_w > 1e-15) {
            for (u64 i = 0; i < N; i++) v[i] = w[i] / norm_w;
        }

        // Convergence check
        if (Math::abs(new_lambda - lambda) < 1e-12 * Math::abs(new_lambda) + 1e-14) {
            lambda = new_lambda;
            break;
        }
        lambda = new_lambda;
    }

    return {lambda, move(v)};
}

} // namespace detail

[[nodiscard]] inline PCA_Result pca(const Vec<Vec<f64>>& data, u64 max_components = 10) noexcept {
    u64 n_obs = data.length();
    u64 n_vars = n_obs > 0 ? data[0].length() : 0;
    if (n_obs < 2 || n_vars == 0) return {};

    // Check consistent dimensions
    for (u64 i = 1; i < n_obs; i++) {
        if (data[i].length() != n_vars) return {};
    }

    u64 comps = Math::min(max_components, n_vars);

    // Compute covariance matrix
    Vec<f64> cov = covariance_matrix(data);
    if (cov.length() == 0) return {};

    Vec<f64> eigenvalues = Vec<f64>::make(comps);
    Vec<Vec<f64>> eigenvectors(comps);
    for (u64 i = 0; i < comps; i++) {
        eigenvectors[i] = Vec<f64>();
    }

    // Deflation-based power iteration: extract components one at a time
    Vec<f64> residual = cov.clone();
    for (u64 comp = 0; comp < comps; comp++) {
        Slice<const f64> mat_slice{residual.data(), residual.length()};
        detail::PowerIterResult pi_res = detail::power_iteration(mat_slice, n_vars);
        f64 eigval = pi_res.eigenvalue;
        Vec<f64> eigvec = move(pi_res.eigenvector);

        eigenvalues[comp] = eigval;
        eigenvectors[comp] = move(eigvec);

        if (comp + 1 < comps) {
            // Deflate: A' = A - lambda * v * v^T
            for (u64 i = 0; i < n_vars; i++) {
                for (u64 j = 0; j < n_vars; j++) {
                    residual[i * n_vars + j] -= eigval * eigenvectors[comp][i] * eigenvectors[comp][j];
                }
            }
        }
    }

    return {move(eigenvalues), move(eigenvectors)};
}

// ============================================================
// Rolling / moving window statistics
// ============================================================

[[nodiscard]] inline f64 rolling_mean(Slice<const f64> data, u64 window, u64 index) noexcept {
    u64 n = data.length();
    if (n == 0 || window == 0 || index >= n) return 0.0;

    u64 start = (index >= window) ? index - window : 0;
    u64 end = index;
    u64 count = end - start + 1;

    if (count == 0) return 0.0;

    f64 sum = 0.0;
    for (u64 i = start; i <= end; i++) {
        sum += data[i];
    }
    return sum / (f64)count;
}

[[nodiscard]] inline f64 rolling_std(Slice<const f64> data, u64 window, u64 index) noexcept {
    u64 n = data.length();
    if (n == 0 || window == 0 || index >= n) return 0.0;

    u64 start = (index >= window) ? index - window : 0;
    u64 end = index;
    u64 count = end - start + 1;

    if (count < 2) return 0.0;

    f64 mean = rolling_mean(data, window, index);

    f64 sum_sq = 0.0;
    for (u64 i = start; i <= end; i++) {
        f64 diff = data[i] - mean;
        sum_sq += diff * diff;
    }
    return Math::sqrt(sum_sq / (f64)(count - 1));
}

} // namespace spp::quant::stat
