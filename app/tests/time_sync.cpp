#include "test.h"

#include <spp/io/stream.h>

#include <binance/time_sync.h>

namespace Bnc = spp::App::Binance;

i32 main() {
    Test test{"empty"_v};

    Trace("Time_Sync.refresh parses serverTime + applies symmetric RTT offset") {
        // Server is exactly 5000 ms ahead of local. With the stubbed clock
        // RTT = 10 ms, so the half-RTT correction is 5 ms and skew should be
        // server_ms + 5 - t1 = 1700000005000 + 5 - 1700000000010 = 4995.
        spp::Net::Memory_Stream wire;
        wire.inject(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 28\r\n"
            "\r\n"
            "{\"serverTime\":1700000005000}"_v);

        // Deterministic clock: first call returns t0, second returns t1.
        i64 stub_times[] = {1700000000000LL, 1700000000010LL};
        u64 stub_idx = 0;
        auto stub_clock = [&]() noexcept -> i64 {
            return stub_times[stub_idx++ < 2 ? stub_idx - 1 : 1];
        };

        Bnc::Time_Sync ts;
        auto rc = ts.refresh_with_clock(wire, "api.binance.com"_v, stub_clock);
        assert(rc.ok());
        assert(ts.rtt_ms == 10);
        assert(ts.skew_ms == 4995);
        assert(ts.last_sample_at_ms == 1700000000010LL);
        assert(ts.samples == 1);
    }

    Trace("timestamp_for applies skew correctly") {
        Bnc::Time_Sync ts;
        ts.skew_ms = 2500;
        assert(ts.timestamp_for(1000) == 3500);
        assert(ts.timestamp_for(0) == 2500);
    }

    Trace("should_refresh respects max age and first-call case") {
        Bnc::Time_Sync ts;
        // Never sampled → must refresh.
        assert(ts.should_refresh(1000));

        ts.last_sample_at_ms = 1000;
        ts.samples = 1;
        // Within max-age window.
        assert(!ts.should_refresh(2000, 60000));
        // Past max-age.
        assert(ts.should_refresh(65000, 60000));
    }

    Trace("refresh propagates HTTP error status as Result error") {
        spp::Net::Memory_Stream wire;
        wire.inject(
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 0\r\n"
            "\r\n"_v);

        Bnc::Time_Sync ts;
        auto rc = ts.refresh_with_clock(wire, "api.binance.com"_v, []() -> i64 { return 0; });
        assert(!rc.ok());
        assert(ts.samples == 0); // no successful sample recorded
    }

    return 0;
}
