#pragma once

#include <spp/io/stream.h>
#include <spp/protocol/http.h>
#include <spp/reflection/json.h>

#include <binance/clock.h>
#include <binance/models.h>

namespace spp::App::Binance {

// Tracks the offset between our local clock and Binance's serverTime.
// Binance signed requests are rejected when |now - serverTime| > recvWindow
// (default 5s), so we periodically refresh and apply the measured skew.
//
// Refresh policy is caller-driven: there is no background thread. Call
// `refresh(stream)` whenever you want a new sample (every 60s is a safe
// default; before any signed request if you've been idle is also fine).
struct Time_Sync {
    // local_ms + skew_ms ≈ server_ms (positive skew means our clock is behind).
    i64 skew_ms = 0;
    i64 rtt_ms = 0;
    i64 last_sample_at_ms = 0;
    u64 samples = 0;

    // Refresh skew by hitting `GET <path>` on the given stream, sampling the
    // local clock via `clock()` immediately before send and right after
    // receive. `clock` defaults to the real wall clock but can be swapped for
    // a deterministic stub in tests.
    template<typename S, typename Clock>
        requires Net::Byte_Stream<S> && Invocable<Clock>
    [[nodiscard]] Result<u64, String_View>
    refresh_with_clock(S& stream, String_View host, Clock&& clock,
                       String_View path = "/api/v3/time"_v) noexcept {
        Protocol::Http::Request<Mdefault> req;
        req.method = "GET"_v;
        req.path = path;
        req.host = host;

        i64 t0 = clock();
        auto fetched = Protocol::Http::fetch(stream, req);
        if(!fetched.ok()) return Result<u64, String_View>::err(spp::move(fetched.unwrap_err()));
        i64 t1 = clock();
        if(fetched.unwrap().response.status_code != 200) {
            return Result<u64, String_View>::err("time_sync_bad_status"_v);
        }

        String_View body{fetched.unwrap().response.body.data(),
                         fetched.unwrap().response.body.length()};
        auto parsed = Json::parse_result<Server_Time>(body);
        if(!parsed.ok()) {
            return Result<u64, String_View>::err(spp::move(parsed.unwrap_err()));
        }

        i64 server_ms = parsed.unwrap().serverTime;
        rtt_ms = t1 - t0;
        // Best estimate of server's clock at t1: serverTime advanced by half RTT.
        skew_ms = server_ms + rtt_ms / 2 - t1;
        last_sample_at_ms = t1;
        samples++;
        return Result<u64, String_View>::ok(static_cast<u64>(skew_ms));
    }

    // Convenience: production path that samples the real wall clock.
    template<typename S>
        requires Net::Byte_Stream<S>
    [[nodiscard]] Result<u64, String_View>
    refresh(S& stream, String_View host,
            String_View path = "/api/v3/time"_v) noexcept {
        return refresh_with_clock(stream, host, [] { return now_ms(); }, path);
    }

    // Returns the timestamp to put in a signed request. Equivalent to local
    // now() + measured skew so the server sees a value within recvWindow.
    [[nodiscard]] i64 timestamp_for(i64 local_now_ms) const noexcept {
        return local_now_ms + skew_ms;
    }

    [[nodiscard]] i64 timestamp() const noexcept {
        return timestamp_for(now_ms());
    }

    // Returns true if a refresh is recommended (sample is stale or never run).
    [[nodiscard]] bool should_refresh(i64 local_now_ms,
                                      i64 max_age_ms = 60000) const noexcept {
        if(samples == 0) return true;
        return local_now_ms - last_sample_at_ms > max_age_ms;
    }
};

} // namespace spp::App::Binance
