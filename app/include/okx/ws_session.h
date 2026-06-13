#pragma once

// WS-layer reconnect wrapper for OKX market / user streams.
//
// `Tls_Session` already handles the byte-transport layer: open the
// socket, reconnect with backoff on failure.  The WS layer above adds
// two extra concerns the transport layer can't see:
//
//   1. After TLS reconnects, the RFC 6455 handshake must run again
//      (the WS protocol state belongs to the closed session).
//   2. OKX forgets subscriptions when the session drops — they must be
//      replayed on the new socket, and the user stream must re-login
//      before sub channels.
//
// `Ws_Session<StreamHolder>` composes:
//   - a Tls_Session (owns TCP+TLS, knows about backoff)
//   - a stream holder (Market_Stream or User_Stream) that wraps Ws_Client
//   - a "post-open" callback that re-issues subscribe / login
//
// Recv path is `recv_or_reconnect(now_ms)`:
//   - If healthy → drain one WS message.
//   - On recv error or EOF → mark disconnected; subsequent calls run
//     try_reconnect_ which: closes Ws_Client state, re-handshakes TLS
//     (Tls_Session::try_connect with backoff), Ws_Client::reset(),
//     Ws_Client::handshake(), then the post-open callback. The
//     reconnect counter increments; if the backoff window hasn't
//     elapsed, recv_or_reconnect returns `backoff_pending` so the
//     event loop can yield.

#include <spp/core/base.h>
#include <spp/core/result.h>

#include <binance/tls_session.h>
#include <binance/ws_client.h>

#include <okx/market_stream.h>
#include <okx/user_stream.h>

namespace spp::App::Okx {

using Tls_Session = App::Binance::Tls_Session;
using Backoff_Config = App::Binance::Backoff_Config;

// Hold the WS endpoint params + post-open replay callback.
struct Ws_Endpoint {
    String_View host = k_public_ws_host;
    String_View path = k_public_ws_path;
};

// A holder concept: provides `.ws` (Ws_Client), `.resubscribe_all()` or
// `.relogin_and_resubscribe()` for re-playing application state.
template<typename H>
concept Has_Ws_Holder = requires(H& h, i64 ms) {
    { h.ws.handshake(""_v, ""_v) } -> Same<Result<u64, String_View>>;
    { h.ws.reset() } -> Same<void>;
};

// Market_Stream / User_Stream specialise the post-open replay via
// `Replay` — a small callable taking (Holder&, i64 now_ms) and
// returning Result<u64, String_View>.  Market re-issues subscribe;
// user re-logins then re-subscribes.
//
// `Connector` is the transport layer: production passes `Tls_Session`
// (mbedTLS-backed); tests pass a stub that pretends the underlying
// stream is always alive so the WS-layer reconnect can be exercised
// without a real TLS handshake.
template<typename Holder, typename Replay, typename Connector = Tls_Session>
struct Ws_Session {
    Connector& tls;
    Holder& holder;
    Replay replay;
    Ws_Endpoint endpoint;

    // Tracks WS-handshake state separately from tls.handshake_done():
    // both have to be ok for the session to be usable.
    bool ws_open_ = false;
    u64 reconnects = 0;

    // WS-layer backoff (independent of TLS-layer backoff; a successful
    // TLS reconnect followed by a WS-handshake failure should still
    // pace its retries).
    Backoff_Config backoff;
    i64 next_ws_try_at_ = 0;
    i64 current_ws_backoff_ms_ = 0;
    String_View last_reconnect_err_;

    // OKX keepalive policy.  The server drops idle sessions at 30s;
    // we send a `ping` text frame whenever the connection has been
    // quiet (no recv, no send) for `ping_interval_ms`.  25s leaves a
    // 5s margin for network jitter.  The reply (`pong`) is collapsed
    // by `recv_or_reconnect` so the caller never sees it.
    i64 ping_interval_ms = 25 * 1000;

    // Wall-clock timestamp of the last successful activity (send or
    // recv) on the WS.  `maybe_ping` uses this to decide if a ping is
    // due; `recv_or_reconnect` refreshes it on each non-empty recv.
    i64 last_activity_at_ms_ = 0;
    u64 pings_sent = 0;
    u64 pongs_received = 0;

    Ws_Session(Connector& t, Holder& h, Replay r,
                Ws_Endpoint ep = {}, Backoff_Config bc = {}) noexcept
        : tls(t), holder(h), replay(spp::move(r)), endpoint(ep),
          backoff(spp::move(bc)),
          current_ws_backoff_ms_(bc.initial_ms) {
    }

    // Open the session for the first time.  Caller invokes once at
    // startup; afterwards `recv_or_reconnect` handles the reconnect
    // cycle internally.
    [[nodiscard]] Result<u64, String_View> open_initial(i64 now_ms) noexcept {
        auto t = tls.try_connect(now_ms);
        if(!t.ok()) return Result<u64, String_View>::err(spp::move(t.unwrap_err()));
        if(t.unwrap() == -1) {
            return Result<u64, String_View>::err("backoff_pending"_v);
        }
        holder.ws.reset();
        auto h = holder.ws.handshake(endpoint.host, endpoint.path);
        if(!h.ok()) return Result<u64, String_View>::err(spp::move(h.unwrap_err()));
        auto r = replay(holder, now_ms);
        if(!r.ok()) return Result<u64, String_View>::err(spp::move(r.unwrap_err()));
        ws_open_ = true;
        current_ws_backoff_ms_ = backoff.initial_ms;
        return Result<u64, String_View>::ok(0);
    }

    // Send a keepalive `ping` text frame if the session has been
    // silent for at least `ping_interval_ms`.  OKX expects literal
    // text "ping" (NOT an RFC 6455 PING opcode) and replies with
    // text "pong".  Returns ok(1) if a ping was emitted, ok(0) if
    // no ping was due (still inside the activity window), err if
    // the send failed (which transitions the session into the
    // reconnect state on the next call).
    [[nodiscard]] Result<u64, String_View> maybe_ping(i64 now_ms) noexcept {
        if(!ws_open_) return Result<u64, String_View>::ok(0);
        // First-ever check: arm the timer at `now_ms` and skip — we
        // don't want to ping immediately after connect.
        if(last_activity_at_ms_ == 0) {
            last_activity_at_ms_ = now_ms;
            return Result<u64, String_View>::ok(0);
        }
        if(now_ms - last_activity_at_ms_ < ping_interval_ms) {
            return Result<u64, String_View>::ok(0);
        }
        auto r = holder.ws.send_text("ping"_v);
        if(!r.ok()) {
            ws_open_ = false;
            last_reconnect_err_ = r.unwrap_err();
            return Result<u64, String_View>::err(spp::move(r.unwrap_err()));
        }
        pings_sent++;
        last_activity_at_ms_ = now_ms;
        return Result<u64, String_View>::ok(1);
    }

    // Pull one WS message; if the underlying recv reports EOF or any
    // error, transition into the reconnect state. The caller's next
    // call will see either a fresh message (post-reconnect) or a
    // `backoff_pending` error to indicate the loop should yield.
    //
    // `pong` replies to our keepalive pings are silently swallowed —
    // the caller receives an empty payload instead, matching how
    // Bar_Aggregator already treats heartbeat frames.
    [[nodiscard]] Result<Vec<u8, Mdefault>, String_View>
    recv_or_reconnect(i64 now_ms) noexcept {
        if(!ws_open_) {
            auto r = try_reconnect_(now_ms);
            if(!r.ok()) {
                return Result<Vec<u8, Mdefault>, String_View>::err(
                    spp::move(r.unwrap_err()));
            }
        }
        auto msg = holder.ws.recv_message();
        if(!msg.ok()) {
            // Mark disconnected and surface the err — caller may try
            // again on the next tick (this time we'll hit the
            // try_reconnect_ branch above).
            ws_open_ = false;
            last_reconnect_err_ = msg.unwrap_err();
            return Result<Vec<u8, Mdefault>, String_View>::err(
                spp::move(msg.unwrap_err()));
        }
        auto& bytes = msg.unwrap();
        // Activity refresh on any successful recv, even an empty one.
        last_activity_at_ms_ = now_ms;
        if(bytes.length() == 4 &&
           bytes[0] == 'p' && bytes[1] == 'o' &&
           bytes[2] == 'n' && bytes[3] == 'g') {
            pongs_received++;
            bytes = Vec<u8, Mdefault>{};   // collapse to empty payload
        }
        return msg;
    }

    [[nodiscard]] bool is_open() const noexcept {
        return ws_open_;
    }

private:
    [[nodiscard]] Result<u64, String_View> try_reconnect_(i64 now_ms) noexcept {
        // 1. Ensure TLS is up.  Tls_Session handles its own backoff;
        //    when it returns -1 we propagate so the caller yields.
        auto t = tls.try_connect(now_ms);
        if(!t.ok()) {
            last_reconnect_err_ = t.unwrap_err();
            return Result<u64, String_View>::err(spp::move(t.unwrap_err()));
        }
        if(t.unwrap() == -1) {
            return Result<u64, String_View>::err("backoff_pending"_v);
        }

        // 2. WS-layer backoff. Tls_Session might be happily connected
        //    yet the WS handshake itself keeps failing — pace those.
        if(now_ms < next_ws_try_at_) {
            return Result<u64, String_View>::err("backoff_pending"_v);
        }

        // 3. Reset Ws_Client state and re-handshake.
        holder.ws.reset();
        auto h = holder.ws.handshake(endpoint.host, endpoint.path);
        if(!h.ok()) {
            install_ws_backoff_(now_ms);
            last_reconnect_err_ = h.unwrap_err();
            return Result<u64, String_View>::err(spp::move(h.unwrap_err()));
        }

        // 4. Re-issue subscribes / login.
        auto r = replay(holder, now_ms);
        if(!r.ok()) {
            install_ws_backoff_(now_ms);
            last_reconnect_err_ = r.unwrap_err();
            return Result<u64, String_View>::err(spp::move(r.unwrap_err()));
        }

        ws_open_ = true;
        reconnects++;
        current_ws_backoff_ms_ = backoff.initial_ms;
        return Result<u64, String_View>::ok(0);
    }

    void install_ws_backoff_(i64 now_ms) noexcept {
        next_ws_try_at_ = now_ms + current_ws_backoff_ms_;
        f64 next = static_cast<f64>(current_ws_backoff_ms_) * backoff.multiplier;
        if(next > static_cast<f64>(backoff.max_ms)) {
            next = static_cast<f64>(backoff.max_ms);
        }
        current_ws_backoff_ms_ = static_cast<i64>(next);
    }
};

// Helper factory: produces the canonical replay callback for a
// Market_Stream — just calls `resubscribe_all` after the new handshake.
template<typename S>
[[nodiscard]] inline auto make_market_replay() noexcept {
    return [](Market_Stream<S>& m, i64 /*now_ms*/) noexcept
           -> Result<u64, String_View> {
        return m.resubscribe_all();
    };
}

// Helper factory: produces the canonical replay callback for a
// User_Stream — re-login then re-subscribe.
template<typename S>
[[nodiscard]] inline auto make_user_replay() noexcept {
    return [](User_Stream<S>& u, i64 now_ms) noexcept
           -> Result<u64, String_View> {
        return u.relogin_and_resubscribe(now_ms);
    };
}

} // namespace spp::App::Okx
