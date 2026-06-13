#pragma once

// Rate limiter for OKX V5.
//
// OKX rate limits are PER-ENDPOINT — "60 reqs / 2s" on /api/v5/trade/order,
// "10 reqs / 2s" on /api/v5/account/balance, and so on.  The Binance
// limiter is a global weight bucket, which doesn't map cleanly, but its
// shape (sliding window + cooldown + Retry-After) is reusable.
//
// We re-export it under the OKX namespace pre-configured with a single
// "60 reqs / 2s" bucket — conservative enough that no single endpoint
// blows through it on its own.  Operators with high-throughput needs
// can construct their own per-endpoint limiter and pass it explicitly.

#include <binance/rate_limiter.h>

namespace spp::App::Okx {

struct Rate_Limiter : App::Binance::Rate_Limiter {
    Rate_Limiter() noexcept : App::Binance::Rate_Limiter(60) {
        // Override the default 1M bucket with a 2-second window so the
        // "60 reqs / 2s" cap matches the most restrictive OKX endpoint.
        buckets[0].interval = "2s"_v;
        buckets[0].limit = 60;
        buckets[0].window_ms = 2000;
    }
};

} // namespace spp::App::Okx
