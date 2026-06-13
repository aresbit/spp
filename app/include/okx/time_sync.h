#pragma once

// OKX server clock skew tracker.
//
// OKX rejects any signed request whose OK-ACCESS-TIMESTAMP differs from
// the server's wall clock by more than ~30 seconds (the threshold is
// documented as "approximate" and tends to tighten under heavy load).
// Whenever the local box's clock drifts past that window every signed
// REST call will start returning {"code":"50114","msg":"Invalid OK-ACCESS-TIMESTAMP"}.
//
// Time_Sync samples `/api/v5/public/time` to measure the offset and
// `timestamp_for(local_now_ms)` returns a corrected value the signer
// can feed into `iso8601_ms()`.
//
// Refresh policy is caller-driven (no background thread).  60 seconds
// between samples is a safe default — the skew drifts on the order of
// microseconds per second so even an hour-stale sample stays well within
// the 30-second window.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/io/stream.h>
#include <spp/protocol/http.h>
#include <spp/reflection/json.h>

#include <okx/clock.h>
#include <okx/models.h>

namespace spp::App::Okx {

struct Time_Sync {
    i64 skew_ms = 0;             // server_ms ≈ local_ms + skew_ms
    i64 rtt_ms = 0;
    i64 last_sample_at_ms = 0;
    u64 samples = 0;

    // Sample OKX's `/api/v5/public/time`, recording the round-trip
    // duration and the inferred skew. `clock` is wired so tests can
    // inject a deterministic local-time source.
    template<typename S, typename Clock>
        requires Net::Byte_Stream<S> && Invocable<Clock>
    [[nodiscard]] Result<u64, String_View>
    refresh_with_clock(S& stream, String_View host, Clock&& clock,
                       String_View path = "/api/v5/public/time"_v) noexcept {
        Protocol::Http::Request<Mdefault> req;
        req.method = "GET"_v;
        req.path = path;
        req.host = host;

        i64 t0 = clock();
        auto fetched = Protocol::Http::fetch(stream, req);
        if(!fetched.ok()) {
            return Result<u64, String_View>::err(spp::move(fetched.unwrap_err()));
        }
        i64 t1 = clock();
        if(fetched.unwrap().response.status_code != 200) {
            return Result<u64, String_View>::err("time_sync_bad_status"_v);
        }

        String_View body{fetched.unwrap().response.body.data(),
                         fetched.unwrap().response.body.length()};
        auto parsed = Json::parse_result<Server_Time_Resp>(body);
        if(!parsed.ok()) {
            return Result<u64, String_View>::err(spp::move(parsed.unwrap_err()));
        }
        auto& resp = parsed.unwrap();
        if(resp.code != "0"_v || resp.data.length() == 0) {
            return Result<u64, String_View>::err("time_sync_no_data"_v);
        }
        // OKX returns "ts" as a decimal string of ms since epoch.
        i64 server_ms = 0;
        for(u8 c : resp.data[0].ts.view()) {
            if(c >= '0' && c <= '9') server_ms = server_ms * 10 + (c - '0');
        }

        rtt_ms = t1 - t0;
        skew_ms = server_ms + rtt_ms / 2 - t1;
        last_sample_at_ms = t1;
        samples++;
        return Result<u64, String_View>::ok(static_cast<u64>(skew_ms));
    }

    // Production refresh — reads the real wall clock from `now_ms`.
    template<typename S>
        requires Net::Byte_Stream<S>
    [[nodiscard]] Result<u64, String_View>
    refresh(S& stream, String_View host,
            String_View path = "/api/v5/public/time"_v) noexcept {
        return refresh_with_clock(stream, host, [] { return now_ms(); }, path);
    }

    // Apply the cached skew to a local wall-clock value.  Pass this
    // into the OKX signer instead of raw `now_ms()` so the encoded
    // timestamp matches what the server will accept.
    [[nodiscard]] i64 timestamp_for(i64 local_now_ms) const noexcept {
        return local_now_ms + skew_ms;
    }

    [[nodiscard]] i64 timestamp() const noexcept {
        return timestamp_for(now_ms());
    }

    [[nodiscard]] bool should_refresh(i64 local_now_ms,
                                       i64 max_age_ms = 60000) const noexcept {
        if(samples == 0) return true;
        return local_now_ms - last_sample_at_ms > max_age_ms;
    }
};

} // namespace spp::App::Okx
