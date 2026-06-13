#pragma once

#include <spp/core/base.h>

#include <sys/time.h>

namespace spp::App::Binance {

// Milliseconds since the Unix epoch. Binance signed-endpoint timestamps and
// recvWindow are both ms-precision, so this is the single time unit the
// integration layer speaks. Pulled out as a free function so tests can inject
// deterministic timestamps via parameters instead of mocking the clock.
[[nodiscard]] inline i64 now_ms() noexcept {
    timeval tv{};
    static_cast<void>(gettimeofday(&tv, nullptr));
    return static_cast<i64>(tv.tv_sec) * 1000 + static_cast<i64>(tv.tv_usec) / 1000;
}

} // namespace spp::App::Binance
