#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

namespace spp::quant {

// =========================================================================
// FactorSensitivity — sensitivity of a single instrument to a single risk factor
// =========================================================================
struct FactorSensitivity {
    String_View factor_name_  = ""_v;
    f64         sensitivity_  = 0.0;  ///< dV / dFactor (partial derivative)
    f64         contribution_ = 0.0;  ///< sensitivity * factor_value (risk contribution)

    SPP_RECORD(FactorSensitivity, SPP_FIELD(factor_name_),
               SPP_FIELD(sensitivity_), SPP_FIELD(contribution_));
};

// =========================================================================
// SensitivityMatrix — instruments (rows) x risk factors (columns)
//
// Stored as [instrument][factor] = sensitivity of instrument i to factor j.
// =========================================================================
struct SensitivityMatrix {
    Vec<String_View> instrument_names_;
    Vec<String_View> factor_names_;
    Vec<Vec<f64>>    matrix_;  ///< matrix_[i][j] = dV_i / dFactor_j

    // -----------------------------------------------------------------
    // total_by_factor — total portfolio sensitivity to each risk factor
    // Sum of absolute sensitivity across instruments, per factor column.
    // -----------------------------------------------------------------
    [[nodiscard]] Vec<FactorSensitivity> total_by_factor() const noexcept {
        u64 n_instr = instrument_names_.length();
        u64 n_factors = factor_names_.length();
        Vec<FactorSensitivity> result = Vec<FactorSensitivity>::make(n_factors);

        for (u64 j = 0; j < n_factors; j++) {
            result[j].factor_name_ = factor_names_[j];
            result[j].sensitivity_ = 0.0;
            for (u64 i = 0; i < n_instr; i++) {
                if (i < matrix_.length() && j < matrix_[i].length()) {
                    result[j].sensitivity_ += matrix_[i][j];
                }
            }
            result[j].contribution_ = result[j].sensitivity_;
        }

        return result;
    }

    // -----------------------------------------------------------------
    // total_by_instrument — total sensitivity of each instrument across
    // all risk factors.
    // -----------------------------------------------------------------
    [[nodiscard]] Vec<FactorSensitivity> total_by_instrument() const noexcept {
        u64 n_instr = instrument_names_.length();
        u64 n_factors = factor_names_.length();
        Vec<FactorSensitivity> result = Vec<FactorSensitivity>::make(n_instr);

        for (u64 i = 0; i < n_instr; i++) {
            result[i].factor_name_ = instrument_names_[i];
            result[i].sensitivity_ = 0.0;
            if (i < matrix_.length()) {
                u64 nf = matrix_[i].length();
                for (u64 j = 0; j < nf && j < n_factors; j++) {
                    result[i].sensitivity_ += matrix_[i][j];
                }
            }
            result[i].contribution_ = result[i].sensitivity_;
        }

        return result;
    }

    // -----------------------------------------------------------------
    // marginal_contributions — Marginal Contribution to Total Risk (MCTR)
    //
    // For each instrument i and factor j, computes:
    //   MC(i,j) = sensitivity(i,j) / sum_k |sensitivity(k,j)|
    //
    // This gives the fraction of total factor-j risk attributable to
    // instrument i.  Returns matrix of same dimensions.
    // -----------------------------------------------------------------
    [[nodiscard]] Vec<Vec<f64>> marginal_contributions() const noexcept {
        u64 n_instr = instrument_names_.length();
        u64 n_factors = factor_names_.length();

        Vec<Vec<f64>> result(n_instr);
        for (u64 i = 0; i < n_instr; i++) {
            result[i] = Vec<f64>::make(n_factors);
            for (u64 j = 0; j < n_factors; j++) {
                result[i][j] = 0.0;
            }
        }

        // Compute total absolute sensitivity per factor
        Vec<f64> total_abs = Vec<f64>::make(n_factors);
        for (u64 j = 0; j < n_factors; j++) {
            total_abs[j] = 0.0;
            for (u64 i = 0; i < n_instr; i++) {
                if (i < matrix_.length() && j < matrix_[i].length()) {
                    total_abs[j] += Math::abs(matrix_[i][j]);
                }
            }
        }

        // Compute marginal contributions
        for (u64 i = 0; i < n_instr; i++) {
            if (i >= matrix_.length()) continue;
            for (u64 j = 0; j < n_factors; j++) {
                if (j >= matrix_[i].length()) continue;
                if (total_abs[j] > 1e-15) {
                    result[i][j] = matrix_[i][j] / total_abs[j];
                }
            }
        }

        return result;
    }

    SPP_RECORD(SensitivityMatrix, SPP_FIELD(instrument_names_),
               SPP_FIELD(factor_names_), SPP_FIELD(matrix_));
};

}  // namespace spp::quant
