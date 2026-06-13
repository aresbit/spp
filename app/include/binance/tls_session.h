#pragma once

// TLS connection management with exponential-backoff reconnect.
//
// Two-layer responsibility:
//
//   1. Underlying TLS stream (`Tls_Mbedtls_Stream`) handles bytes.
//   2. `Tls_Session` owns the stream + tracks when to reconnect and
//      how long to wait between attempts.
//
// `Tls_Session` is intentionally NOT templated on the stream type — it
// hard-binds to `Ext::Tls_Mbedtls_Stream` because there's no portable
// reconnect API across arbitrary `Byte_Stream`s. Tests that need a
// mock TLS stream should pump `Tls_Mbedtls_Stream` against a localhost
// stunnel rather than try to template this layer.
//
// This header is opt-in (#include only when needed) so the rest of the
// integration doesn't grow a hard dependency on mbedTLS.
//
// listenKey keepalive is a separate concern (Binance requires PUT
// /api/v3/userDataStream every <30 min). `keepalive_due(now_ms)` tells
// the caller when to re-issue; we don't run the timer because the
// caller may have its own scheduler.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/ext/tls_mbedtls.h>

namespace spp::App::Binance {

struct Backoff_Config {
    i64 initial_ms = 1000;          // first sleep after a failed connect
    i64 max_ms     = 30000;         // cap so we don't sleep forever
    f64 multiplier = 2.0;           // doubling by default
    i64 listen_key_keepalive_ms = 25 * 60 * 1000; // re-PUT every 25 min
};

struct Tls_Session {
    Ext::Tls_Mbedtls_Stream tls;

    String<Mdefault> host;
    u16 port = 443;
    Backoff_Config backoff;

    // Monotonic next-attempt threshold. The caller's event loop is
    // expected to skip reconnect attempts while `now_ms < next_try_at_`.
    i64 next_try_at_ = 0;
    i64 current_backoff_ms_ = 0;
    i64 last_keepalive_ms_ = 0;
    u64 connect_attempts = 0;
    u64 connect_successes = 0;

    Tls_Session() noexcept = default;
    Tls_Session(String_View h, u16 p, Backoff_Config bc = {}) noexcept
        : host(h.template string<Mdefault>()),
          port(p),
          backoff(spp::move(bc)),
          current_backoff_ms_(bc.initial_ms) {
    }

    // Try once to (re-)establish the TLS session. Returns ok on success,
    // err on a failed handshake. The caller decides whether to back off
    // and retry; see `try_connect(now_ms)` for the timed-gate variant.
    [[nodiscard]] Result<u64, String_View>
    connect_now() noexcept {
        connect_attempts++;
        if(tls.handshake_done()) {
            tls.close();
        }
        auto r = tls.connect_result(host.view(), port);
        if(r.ok()) {
            connect_successes++;
            current_backoff_ms_ = backoff.initial_ms;
            return Result<u64, String_View>::ok(0);
        }
        return Result<u64, String_View>::err(spp::move(r.unwrap_err()));
    }

    // Timed-gate connect: respects `next_try_at_` so an event loop that
    // calls it every tick doesn't hammer the exchange after a failure.
    // Returns:
    //   ok(1)        — newly connected this call
    //   ok(0)        — already connected, nothing to do
    //   ok(-1)       — backoff window not yet elapsed, caller should wait
    //   err          — the connect attempt failed
    [[nodiscard]] Result<i64, String_View>
    try_connect(i64 now_ms) noexcept {
        if(tls.handshake_done()) return Result<i64, String_View>::ok(0);
        if(now_ms < next_try_at_) return Result<i64, String_View>::ok(-1);

        auto r = connect_now();
        if(r.ok()) {
            last_keepalive_ms_ = now_ms;
            return Result<i64, String_View>::ok(1);
        }

        // Failed — install backoff and re-publish the deadline.
        next_try_at_ = now_ms + current_backoff_ms_;
        f64 next = static_cast<f64>(current_backoff_ms_) * backoff.multiplier;
        if(next > static_cast<f64>(backoff.max_ms)) {
            next = static_cast<f64>(backoff.max_ms);
        }
        current_backoff_ms_ = static_cast<i64>(next);
        return Result<i64, String_View>::err(spp::move(r.unwrap_err()));
    }

    // Mark the session as having received a keepalive ack so the next
    // due-check is `now_ms + keepalive_interval`. Caller invokes after
    // a successful PUT /api/v3/userDataStream.
    void mark_keepalive(i64 now_ms) noexcept {
        last_keepalive_ms_ = now_ms;
    }

    [[nodiscard]] bool keepalive_due(i64 now_ms) const noexcept {
        return last_keepalive_ms_ > 0 &&
               (now_ms - last_keepalive_ms_) >= backoff.listen_key_keepalive_ms;
    }
};

} // namespace spp::App::Binance
