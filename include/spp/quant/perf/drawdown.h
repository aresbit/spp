#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include <cmath>

namespace spp::quant {

// =========================================================================
// DrawdownResult — drawdown analysis from an equity curve
//
// All drawdown values are stored as positive fractions (e.g. 0.15 = 15%).
// =========================================================================
struct DrawdownResult {
    f64       max_drawdown          = 0.0;   ///< Maximum peak-to-trough decline
    f64       max_drawdown_duration = 0.0;   ///< Duration of max drawdown (in periods)
    f64       avg_drawdown          = 0.0;   ///< Average drawdown across all periods
    u64       drawdown_start_idx    = 0;     ///< Index where max drawdown started
    u64       drawdown_end_idx      = 0;     ///< Index where max drawdown trough occurred
    Vec<f64>  drawdown_series;               ///< Drawdown at each period (positive = decline)

    SPP_RECORD(DrawdownResult, SPP_FIELD(max_drawdown),
               SPP_FIELD(max_drawdown_duration), SPP_FIELD(avg_drawdown),
               SPP_FIELD(drawdown_start_idx), SPP_FIELD(drawdown_end_idx));
};

// =========================================================================
// compute_drawdowns — compute drawdown series from an equity curve
//
// Drawdown at time t = (peak_to_date - V_t) / peak_to_date
// where peak_to_date is the highest equity value observed up to t.
//
// The result is always a positive number: 0.15 means a 15% drawdown.
// =========================================================================
inline DrawdownResult compute_drawdowns(Slice<const f64> equity_curve) noexcept {
    u64 n = equity_curve.length();
    DrawdownResult result;

    if (n == 0) {
        result.drawdown_series = Vec<f64>();
        return result;
    }

    result.drawdown_series = Vec<f64>::make(n);

    f64 peak       = equity_curve[0];
    f64 max_dd     = 0.0;
    f64 sum_dd     = 0.0;
    u64  dd_start  = 0;
    u64  dd_end    = 0;
    u64  cur_start = 0;
    f64 cur_dd     = 0.0;
    u64 max_dd_duration = 0;
    u64 cur_dd_duration = 0;

    for (u64 i = 0; i < n; i++) {
        f64 val = equity_curve[i];

        // Update running peak
        if (val > peak) {
            peak = val;
            cur_start = i;
            cur_dd = 0.0;
            cur_dd_duration = 0;
        }

        // Compute current drawdown
        if (peak > 0.0) {
            f64 dd = (peak - val) / peak;
            result.drawdown_series[i] = dd;
            sum_dd += dd;

            if (dd > cur_dd) {
                cur_dd = dd;
                cur_dd_duration = i - cur_start;
            }

            if (dd > max_dd) {
                max_dd     = dd;
                dd_start   = cur_start;
                dd_end     = i;
                max_dd_duration = i - cur_start;
            }
        } else {
            result.drawdown_series[i] = 0.0;
        }
    }

    result.max_drawdown           = max_dd;
    result.max_drawdown_duration  = static_cast<f64>(max_dd_duration);
    result.avg_drawdown           = sum_dd / static_cast<f64>(n);
    result.drawdown_start_idx     = dd_start;
    result.drawdown_end_idx       = dd_end;

    return result;
}

// =========================================================================
// compute_drawdowns_from_returns — compute drawdown from a return series
//
// First builds an equity curve from the return series:
//   equity[t] = initial_value * prod_{s=0}^{t-1} (1 + r_s)
// Then delegates to compute_drawdowns.
// =========================================================================
inline DrawdownResult compute_drawdowns_from_returns(Slice<const f64> returns,
                                                      f64 initial_value = 1.0) noexcept {
    u64 n = returns.length();
    if (n == 0) {
        DrawdownResult empty;
        empty.drawdown_series = Vec<f64>();
        return empty;
    }

    // Build equity curve: n+1 points (starting at initial_value)
    Vec<f64> equity = Vec<f64>::make(n + 1);
    equity[0] = initial_value;
    for (u64 i = 0; i < n; i++) {
        equity[i + 1] = equity[i] * (1.0 + returns[i]);
    }

    DrawdownResult result = compute_drawdowns(equity.slice());

    // Trim the drawdown series to match the return length (remove initial zero)
    // Drawdown at time t (for return t-1) maps to equity index t
    if (result.drawdown_series.length() == n + 1) {
        Vec<f64> trimmed = Vec<f64>::make(n);
        for (u64 i = 0; i < n; i++) {
            trimmed[i] = result.drawdown_series[i + 1];
        }
        result.drawdown_series = spp::move(trimmed);
    }

    return result;
}

}  // namespace spp::quant
