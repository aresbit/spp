#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <cmath>

namespace spp::quant::linalg {

// ============================================================
// Minimal dense matrix for portfolio optimization and statistics
// ============================================================
// Row-major storage in a flat Vec<f64>. Supports basic operations,
// Cholesky decomposition, solve, determinant, and inverse.

template<typename A = Mdefault>
struct Matrix {
    u64 rows_, cols_;
    Vec<f64, A> data_;  // row-major: data_[i * cols_ + j] = A(i, j)

    // Factory methods
    [[nodiscard]] static Matrix zeros(u64 r, u64 c) noexcept {
        Matrix m;
        m.rows_ = r;
        m.cols_ = c;
        m.data_ = Vec<f64, A>::make(r * c);
        for (u64 i = 0; i < r * c; i++) m.data_[i] = 0.0;
        return m;
    }

    [[nodiscard]] static Matrix identity(u64 n) noexcept {
        Matrix m = zeros(n, n);
        for (u64 i = 0; i < n; i++) {
            m.data_[i * n + i] = 1.0;
        }
        return m;
    }

    [[nodiscard]] static Matrix from_vec(u64 r, u64 c, Vec<f64, A> data) noexcept {
        Matrix m;
        m.rows_ = r;
        m.cols_ = c;
        m.data_ = spp::move(data);
        return m;
    }

    // Element access
    [[nodiscard]] f64& operator()(u64 i, u64 j) noexcept {
        return data_[i * cols_ + j];
    }

    [[nodiscard]] f64 operator()(u64 i, u64 j) const noexcept {
        return data_[i * cols_ + j];
    }

    // Row / column views
    [[nodiscard]] Slice<f64> row(u64 i) noexcept {
        return Slice<f64>{data_.data() + i * cols_, cols_};
    }

    [[nodiscard]] Slice<const f64> row(u64 i) const noexcept {
        return Slice<const f64>{data_.data() + i * cols_, cols_};
    }

    [[nodiscard]] Vec<f64, A> col(u64 j) const noexcept {
        Vec<f64, A> result = Vec<f64, A>::make(rows_);
        for (u64 i = 0; i < rows_; i++) {
            result[i] = data_[i * cols_ + j];
        }
        return result;
    }

    // Basic arithmetic
    [[nodiscard]] Matrix operator+(const Matrix& other) const noexcept {
        if (rows_ != other.rows_ || cols_ != other.cols_) return Matrix{};
        Matrix m = zeros(rows_, cols_);
        for (u64 i = 0; i < rows_ * cols_; i++) {
            m.data_[i] = data_[i] + other.data_[i];
        }
        return m;
    }

    [[nodiscard]] Matrix operator-(const Matrix& other) const noexcept {
        if (rows_ != other.rows_ || cols_ != other.cols_) return Matrix{};
        Matrix m = zeros(rows_, cols_);
        for (u64 i = 0; i < rows_ * cols_; i++) {
            m.data_[i] = data_[i] - other.data_[i];
        }
        return m;
    }

    [[nodiscard]] Matrix operator*(f64 scalar) const noexcept {
        Matrix m = zeros(rows_, cols_);
        for (u64 i = 0; i < rows_ * cols_; i++) {
            m.data_[i] = data_[i] * scalar;
        }
        return m;
    }

    // Matrix multiply: C(i, k) = sum_j A(i, j) * B(j, k)
    [[nodiscard]] Matrix operator*(const Matrix& other) const noexcept {
        if (cols_ != other.rows_) return Matrix{};
        Matrix m = zeros(rows_, other.cols_);
        for (u64 i = 0; i < rows_; i++) {
            for (u64 k = 0; k < other.cols_; k++) {
                f64 sum = 0.0;
                for (u64 j = 0; j < cols_; j++) {
                    sum += data_[i * cols_ + j] * other.data_[j * other.cols_ + k];
                }
                m.data_[i * other.cols_ + k] = sum;
            }
        }
        return m;
    }

    // Matrix-vector multiply: y(i) = sum_j A(i, j) * x(j)
    [[nodiscard]] Vec<f64, A> operator*(Slice<const f64> vec) const noexcept {
        if (cols_ != vec.length()) return Vec<f64, A>();
        Vec<f64, A> result = Vec<f64, A>::make(rows_);
        for (u64 i = 0; i < rows_; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < cols_; j++) {
                sum += data_[i * cols_ + j] * vec[j];
            }
            result[i] = sum;
        }
        return result;
    }

    // Transpose
    [[nodiscard]] Matrix transpose() const noexcept {
        Matrix m = zeros(cols_, rows_);
        for (u64 i = 0; i < rows_; i++) {
            for (u64 j = 0; j < cols_; j++) {
                m.data_[j * rows_ + i] = data_[i * cols_ + j];
            }
        }
        return m;
    }

    // ============================================================
    // Cholesky decomposition (Banachiewicz)
    // ============================================================
    // A = L * L^T where L is lower triangular.
    // Input: symmetric positive definite A.
    // Returns L as a Matrix.

    struct Cholesky {
        Matrix L;  // lower triangular: L * L^T = A
    };

    [[nodiscard]] Opt<Cholesky> cholesky() const noexcept {
        if (rows_ != cols_) return {};

        Matrix L = zeros(rows_, cols_);

        for (u64 i = 0; i < rows_; i++) {
            for (u64 j = 0; j <= i; j++) {
                f64 sum = 0.0;
                for (u64 k = 0; k < j; k++) {
                    sum += L.data_[i * cols_ + k] * L.data_[j * cols_ + k];
                }
                if (i == j) {
                    f64 diag = data_[i * cols_ + i] - sum;
                    if (diag <= 0.0) return {};  // not positive definite
                    L.data_[i * cols_ + j] = Math::sqrt(diag);
                } else {
                    L.data_[i * cols_ + j] = (data_[i * cols_ + j] - sum) / L.data_[j * cols_ + j];
                }
            }
        }

        Cholesky chol;
        chol.L = move(L);
        return Opt{move(chol)};
    }

    // ============================================================
    // Solve Ax = b for symmetric positive definite A via Cholesky
    // ============================================================

    [[nodiscard]] Opt<Vec<f64, A>> solve(Slice<const f64> b) const noexcept {
        if (rows_ != cols_ || rows_ != b.length()) return {};
        u64 n = rows_;

        Opt<Cholesky> chol_opt = cholesky();
        if (!chol_opt.ok()) return {};

        const Matrix& L = chol_opt->L;
        Vec<f64, A> x = Vec<f64, A>::make(n);

        // Forward substitution: L * y = b
        Vec<f64, A> y = Vec<f64, A>::make(n);
        for (u64 i = 0; i < n; i++) {
            f64 sum = 0.0;
            for (u64 j = 0; j < i; j++) {
                sum += L.data_[i * cols_ + j] * y[j];
            }
            y[i] = (b[i] - sum) / L.data_[i * cols_ + i];
        }

        // Backward substitution: L^T * x = y
        for (u64 i_i = n; i_i > 0; i_i--) {
            u64 i = i_i - 1;
            f64 sum = 0.0;
            for (u64 j = i + 1; j < n; j++) {
                sum += L.data_[j * cols_ + i] * x[j];  // L^T(i, j) = L(j, i)
            }
            x[i] = (y[i] - sum) / L.data_[i * cols_ + i];
        }

        return x;
    }

    // ============================================================
    // Determinant (of symmetric positive definite matrix)
    // ============================================================

    [[nodiscard]] f64 determinant() const noexcept {
        if (rows_ != cols_) return 0.0;

        Opt<Cholesky> chol_opt = cholesky();
        if (!chol_opt.ok()) {
            // Fallback for non-positive-definite: compute via LU but
            // that requires Gaussian elimination. For now return 0.0.
            return 0.0;
        }

        const Matrix& L = chol_opt->L;
        // det(A) = det(L * L^T) = det(L)^2 = prod(diag(L))^2
        f64 det = 1.0;
        for (u64 i = 0; i < rows_; i++) {
            det *= L.data_[i * cols_ + i];
        }
        return det * det;
    }

    // ============================================================
    // Inverse (via Cholesky solve of identity columns)
    // ============================================================

    [[nodiscard]] Matrix inverse() const noexcept {
        if (rows_ != cols_) return Matrix{};

        Opt<Cholesky> chol_opt = cholesky();
        if (!chol_opt.ok()) return Matrix{};

        const Matrix& L = chol_opt->L;
        u64 n = rows_;

        Matrix inv = zeros(n, n);

        // Solve L * L^T * col_inv = e_i for each column i
        // Step 1: Forward sub L * y = e_i  =>  y = L^{-1} * e_i
        // Step 2: Backward sub L^T * col_inv = y

        for (u64 col_idx = 0; col_idx < n; col_idx++) {
            // Forward substitution: L * y = e_col_idx
            Vec<f64, A> y = Vec<f64, A>::make(n);
            for (u64 i = 0; i < n; i++) {
                f64 sum = 0.0;
                for (u64 j = 0; j < i; j++) {
                    sum += L.data_[i * cols_ + j] * y[j];
                }
                f64 rhs = (i == col_idx) ? 1.0 : 0.0;
                y[i] = (rhs - sum) / L.data_[i * cols_ + i];
            }

            // Backward substitution: L^T * x = y
            for (u64 i_i = n; i_i > 0; i_i--) {
                u64 i = i_i - 1;
                f64 sum = 0.0;
                for (u64 j = i + 1; j < n; j++) {
                    sum += L.data_[j * cols_ + i] * inv.data_[j * n + col_idx];
                }
                inv.data_[i * n + col_idx] = (y[i] - sum) / L.data_[i * cols_ + i];
            }
        }

        return inv;
    }
};

} // namespace spp::quant::linalg
