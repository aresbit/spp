#include "test.h"

#include <spp/io/stream.h>
#include <okx/time_sync.h>

namespace Okx = spp::App::Okx;

// Helper (same shape as okx_api.cpp's): inject a 200 OK response.
static void inject_response(spp::Net::Memory_Stream& wire,
                            spp::String_View body) noexcept {
    spp::Vec<u8, Mdefault> out;
    auto put_lit = [&out](const char* s) {
        while(*s) out.push(static_cast<spp::u8>(*s++));
    };
    auto put_sv = [&out](spp::String_View sv) {
        for(spp::u64 i = 0; i < sv.length(); i++) out.push(sv[i]);
    };
    put_lit("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
    {
        u8 buf[32];
        i32 n = Libc::snprintf(buf, sizeof(buf), "%lu",
                               static_cast<unsigned long>(body.length()));
        for(i32 i = 0; i < n; i++) out.push(buf[i]);
    }
    put_lit("\r\n\r\n");
    put_sv(body);
    wire.inject(out.slice());
}

i32 main() {
    Test test{"empty"_v};

    Trace("refresh_with_clock infers skew from serverTs + half-RTT") {
        // Server says it's at 1_700_000_010_000 ms.  Our deterministic
        // clock returns 1_700_000_000_000 before send and
        // 1_700_000_000_020 after recv — so RTT = 20ms, skew ≈
        // 10_000_010 (server ahead by ~10s + half-rtt correction).
        spp::Net::Memory_Stream wire;
        inject_response(wire,
            R"({"code":"0","msg":"","data":[{"ts":"1700000010000"}]})"_v);

        Okx::Time_Sync ts;
        spp::i64 fake_t = 1700000000000LL;
        i32 call = 0;
        auto clock = [&]() noexcept {
            // t0 = 1700000000000, t1 = 1700000000020
            return call++ == 0 ? fake_t : fake_t + 20;
        };
        auto r = ts.refresh_with_clock(wire, "www.okx.com"_v, clock);
        assert(r.ok());
        // server_ms + rtt/2 - t1 = 1700000010000 + 10 - 1700000000020 = 9990
        assert(ts.skew_ms == 9990);
        assert(ts.rtt_ms == 20);
        assert(ts.samples == 1);
        assert(ts.last_sample_at_ms == fake_t + 20);
    }

    Trace("timestamp_for applies the cached skew") {
        Okx::Time_Sync ts;
        ts.skew_ms = 1234;
        assert(ts.timestamp_for(1000) == 2234);
    }

    Trace("should_refresh fires before first sample and again past max_age") {
        Okx::Time_Sync ts;
        // Never sampled — must refresh.
        assert(ts.should_refresh(1000, 60000));

        ts.samples = 1;
        ts.last_sample_at_ms = 1000;
        // 30s after the sample → still fresh under a 60s window.
        assert(!ts.should_refresh(31000, 60000));
        // 61s after → stale.
        assert(ts.should_refresh(62000, 60000));
    }

    Trace("non-200 response is reported as a clean error") {
        spp::Net::Memory_Stream wire;
        spp::Vec<u8, Mdefault> resp;
        auto push_lit = [&resp](const char* s) {
            while(*s) resp.push(static_cast<spp::u8>(*s++));
        };
        push_lit("HTTP/1.1 503 Service Unavailable\r\n");
        push_lit("Content-Length: 0\r\n\r\n");
        wire.inject(resp.slice());

        Okx::Time_Sync ts;
        auto r = ts.refresh(wire, "www.okx.com"_v);
        assert(!r.ok());
        // Either the http parser surfaces 503 as http_error_status from
        // send_throttled-style guards (we don't use those here — direct
        // fetch), or our own time_sync_bad_status fires. Both are fine.
    }

    Trace("non-zero code in OKX envelope is rejected even on HTTP 200") {
        spp::Net::Memory_Stream wire;
        inject_response(wire,
            R"({"code":"50001","msg":"rate limit","data":[]})"_v);
        Okx::Time_Sync ts;
        auto r = ts.refresh(wire, "www.okx.com"_v);
        assert(!r.ok());
        assert(r.unwrap_err() == "time_sync_no_data"_v);
    }

    return 0;
}
