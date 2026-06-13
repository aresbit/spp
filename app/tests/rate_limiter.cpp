#include "test.h"

#include <binance/rate_limiter.h>

namespace Bnc = spp::App::Binance;
namespace Http = spp::Protocol::Http;

static Http::Response<Mdefault>
build_response(u32 status, const Vec<Http::Header, Mdefault>& headers) noexcept {
    Http::Response<Mdefault> r;
    r.status_code = status;
    r.status_text = "OK"_v;
    for(const auto& h : headers) r.headers.push(h);
    return r;
}

i32 main() {
    Test test{"empty"_v};

    Trace("Bucket starts permissive, advances cleanly within window") {
        Bnc::Rate_Limiter rl(1200);

        // First request at t=0 should be allowed immediately.
        assert(rl.wait_required_ms(10, 0) == 0);

        // Server reports we've used 100 weight in the 1m window — update.
        Vec<Http::Header, Mdefault> hdrs;
        hdrs.push(Http::Header{"X-MBX-USED-WEIGHT-1M"_v, "100"_v});
        auto resp = build_response(200, hdrs);
        rl.update_from(resp, 10, 1000);
        assert(rl.buckets[0].used == 100);

        // Plenty of budget left at t=1000.
        assert(rl.wait_required_ms(50, 1000) == 0);
    }

    Trace("Budget exhaustion forces a wait until window rollover") {
        Bnc::Rate_Limiter rl(100); // tiny limit so we can exhaust in test
        Vec<Http::Header, Mdefault> hdrs;
        hdrs.push(Http::Header{"X-MBX-USED-WEIGHT-1M"_v, "95"_v});

        rl.update_from(build_response(200, hdrs), 1, 1000); // window starts at t=1000

        // A 10-weight request at t=2000 would overshoot 100 → must wait until
        // the 1m window rolls over (started at t=1000, ends at t=61000).
        i64 wait = rl.wait_required_ms(10, 2000);
        assert(wait == 59000);

        // After the window expires, used resets and we're allowed again.
        i64 wait_after = rl.wait_required_ms(10, 61000);
        assert(wait_after == 0);
        assert(rl.buckets[0].used == 0);
    }

    Trace("HTTP 429 with Retry-After installs cooldown") {
        Bnc::Rate_Limiter rl;
        Vec<Http::Header, Mdefault> hdrs;
        hdrs.push(Http::Header{"Retry-After"_v, "3"_v});

        rl.update_from(build_response(429, hdrs), 1, 5000);
        // 3 seconds of cooldown from t=5000.
        assert(rl.cooldown_until_ms == 8000);
        assert(!rl.ip_banned);

        // Any pre-flight inside the cooldown is blocked regardless of weight.
        assert(rl.wait_required_ms(1, 5000) == 3000);
        assert(rl.wait_required_ms(1, 6500) == 1500);

        // After the cooldown, success clears the gate.
        rl.clear_cooldown_if_passed(8000);
        assert(rl.wait_required_ms(1, 8000) == 0);
    }

    Trace("HTTP 418 (IP ban) installs longer cooldown + ban flag") {
        Bnc::Rate_Limiter rl;
        Vec<Http::Header, Mdefault> hdrs;
        // Server can also omit Retry-After for 418; verify the default floor.
        rl.update_from(build_response(418, hdrs), 1, 1000);
        assert(rl.ip_banned);
        assert(rl.cooldown_until_ms == 1000 + 120000);
    }

    return 0;
}
