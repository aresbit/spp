#pragma once

#include <spp/core/base.h>

namespace spp::quant::factor {

namespace detail {

[[nodiscard]] inline auto ln(f64 x) noexcept -> f64 {
    if (x <= 0.0) return -1.0e18;
    f64 y = 0.0;
    if (x > 1.0) {
        while (x > Math::exp(y + 1.0)) y += 1.0;
    } else if (x < 1.0) {
        while (Math::exp(y) > x) y -= 1.0;
    }
    for (u32 i = 0; i < 10; i++) {
        f64 ey = Math::exp(y);
        y = y - 1.0 + x / ey;
    }
    return y;
}

[[nodiscard]] inline auto tanh(f64 x) noexcept -> f64 {
    f64 e2x = Math::exp(2.0 * x);
    return (e2x - 1.0) / (e2x + 1.0);
}

} // namespace detail
} // namespace spp::quant::factor
