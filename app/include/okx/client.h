#pragma once

// Production facade for OKX V5 REST.  Requires SPP_TLS=1 — pulls in
// mbedTLS.
//
// Layers:
//   - Ext::Tls_Mbedtls_Stream    (raw bytes, TLS-terminated)
//      wrapped by
//   - Binance::Tls_Session       (exponential-backoff auto-reconnect)
//      paired with
//   - Time_Sync                  (server clock skew correction)
//      paired with
//   - Signer_Config + Rate_Limiter (per-request signing + throttle)
//
// Each REST method first calls `ensure_connected_(now_ms)` so a dropped
// session re-handshakes (with backoff) before the request, and pulls a
// fresh server-time sample once per minute to keep the OK-ACCESS-TIMESTAMP
// inside OKX's ±30s validity window.
//
// `Tls_Session` itself was originally written under the Binance
// namespace because there's nothing exchange-specific about a TLS
// reconnect timer — we just re-export it under `Okx::Tls_Session` so
// OKX code reads cohesively.

#include <binance/tls_session.h>

#include <okx/api.h>
#include <okx/clock.h>
#include <okx/models.h>
#include <okx/rate_limiter.h>
#include <okx/signer.h>
#include <okx/time_sync.h>

namespace spp::App::Okx {

using Tls_Session = App::Binance::Tls_Session;
using Backoff_Config = App::Binance::Backoff_Config;

struct Client_Config {
    String_View host = "www.okx.com"_v;
    u16 port = 443;
    String_View api_key;
    String_View api_secret;
    String_View passphrase;
    bool simulated_trading = false;
    bool insecure_skip_verify = false;

    // Refresh the time-sync sample whenever the last one is older than
    // this.  60s is a sane default — clock drift on the order of µs/s
    // means a minute-old sample is still inside OKX's ±30s window.
    i64 time_sync_max_age_ms = 60000;

    // Reconnect policy.  Defaults match Binance::Tls_Session: 1s initial,
    // 30s max, x2 backoff on each failure.
    Backoff_Config backoff;
};

struct Client {
    explicit Client(Client_Config cfg) noexcept
        : config(spp::move(cfg)),
          session(config.host, config.port, config.backoff) {
        signer.api_key            = config.api_key;
        signer.api_secret         = config.api_secret;
        signer.passphrase         = config.passphrase;
        signer.simulated_trading  = config.simulated_trading;
    }

    Client_Config config;
    Signer_Config signer;
    Tls_Session session;
    Rate_Limiter limiter;
    Time_Sync time;

    // First connect (no backoff gating).  Subsequent reconnects happen
    // automatically via `ensure_connected_` inside each REST call.
    [[nodiscard]] Result<u64, String_View> connect() noexcept {
        Ext::Tls_Mbedtls_Stream::Options opts;
        opts.insecure_skip_verify = config.insecure_skip_verify;
        // Tls_Session::connect_now would discard the options struct —
        // call the underlying stream directly for the first attempt,
        // and let try_connect take over after that.
        if(session.tls.handshake_done()) session.tls.close();
        session.connect_attempts++;
        auto r = session.tls.connect_result(config.host, config.port, opts);
        if(r.ok()) {
            session.connect_successes++;
            return Result<u64, String_View>::ok(0);
        }
        // Install backoff so a follow-up try_connect respects the
        // failure window.
        session.next_try_at_ = now_ms() + config.backoff.initial_ms;
        session.current_backoff_ms_ = config.backoff.initial_ms;
        return Result<u64, String_View>::err(spp::move(r.unwrap_err()));
    }

    void close() noexcept {
        session.tls.close();
    }

    // Expose the underlying stream so the live driver / event loop
    // can poll its socket.
    [[nodiscard]] Ext::Tls_Mbedtls_Stream& stream() noexcept {
        return session.tls;
    }
    [[nodiscard]] const Ext::Tls_Mbedtls_Stream& stream() const noexcept {
        return session.tls;
    }

    // --- Public endpoints (no signing, no time correction needed) ---

    [[nodiscard]] Result<Server_Time_Resp, String_View> server_time() noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Server_Time_Resp, String_View>::err(spp::move(c.unwrap_err()));
        return fetch_server_time(session.tls, config.host, limiter, nm);
    }

    [[nodiscard]] Result<Ticker_Resp, String_View>
    ticker(String_View instId) noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Ticker_Resp, String_View>::err(spp::move(c.unwrap_err()));
        return fetch_ticker(session.tls, config.host, limiter, nm, instId);
    }

    [[nodiscard]] Result<Vec<Candlestick, Mdefault>, String_View>
    candles(String_View instId, String_View bar, u32 limit = 100) noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Vec<Candlestick, Mdefault>, String_View>::err(spp::move(c.unwrap_err()));
        return fetch_candles(session.tls, config.host, limiter, nm,
                              instId, bar, limit);
    }

    [[nodiscard]] Result<Instruments_Resp, String_View>
    instruments(String_View instType = "SPOT"_v) noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Instruments_Resp, String_View>::err(spp::move(c.unwrap_err()));
        return fetch_instruments(session.tls, config.host, limiter, nm, instType);
    }

    // --- Signed endpoints — go through ensure_connected_ + sync_time_ ---

    [[nodiscard]] Result<Balance_Resp, String_View>
    balance(String_View ccy = ""_v) noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Balance_Resp, String_View>::err(spp::move(c.unwrap_err()));
        auto s = sync_time_if_stale_(nm);
        if(!s.ok()) return Result<Balance_Resp, String_View>::err(spp::move(s.unwrap_err()));
        return fetch_balance(session.tls, config.host, signer, limiter,
                              time.timestamp_for(nm), ccy);
    }

    [[nodiscard]] Result<Place_Order_Resp, String_View>
    place_order(const Order_Request& order) noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Place_Order_Resp, String_View>::err(spp::move(c.unwrap_err()));
        auto s = sync_time_if_stale_(nm);
        if(!s.ok()) return Result<Place_Order_Resp, String_View>::err(spp::move(s.unwrap_err()));
        return App::Okx::place_order(session.tls, config.host, signer, limiter,
                                      time.timestamp_for(nm), order);
    }

    [[nodiscard]] Result<Cancel_Order_Resp, String_View>
    cancel_order(String_View instId, String_View ordId,
                 String_View clOrdId = ""_v) noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return Result<Cancel_Order_Resp, String_View>::err(spp::move(c.unwrap_err()));
        auto s = sync_time_if_stale_(nm);
        if(!s.ok()) return Result<Cancel_Order_Resp, String_View>::err(spp::move(s.unwrap_err()));
        return App::Okx::cancel_order(session.tls, config.host, signer, limiter,
                                       time.timestamp_for(nm),
                                       instId, ordId, clOrdId);
    }

    // Force a fresh time sync regardless of staleness — useful right
    // after a reconnect or if the caller suspects clock drift.
    [[nodiscard]] Result<u64, String_View> sync_time_now() noexcept {
        i64 nm = now_ms();
        auto c = ensure_connected_(nm);
        if(!c.ok()) return c;
        return time.refresh(session.tls, config.host);
    }

private:
    // Pull the connection back up if dropped, respecting the session's
    // backoff window.  Returns ok(0) when connected; err otherwise.
    [[nodiscard]] Result<u64, String_View> ensure_connected_(i64 nm) noexcept {
        if(session.tls.handshake_done()) return Result<u64, String_View>::ok(0);
        auto r = session.try_connect(nm);
        if(!r.ok()) return Result<u64, String_View>::err(spp::move(r.unwrap_err()));
        // -1 = "still in backoff window"; the caller's request can't be
        // serviced this instant.  Surface as an error so the caller
        // doesn't try to ship bytes through a closed socket.
        if(r.unwrap() == -1) return Result<u64, String_View>::err("backoff_pending"_v);
        // ok(1) means a fresh connection was established — force a new
        // time-sync sample on the next call.
        if(r.unwrap() == 1) time.last_sample_at_ms = 0;
        return Result<u64, String_View>::ok(0);
    }

    [[nodiscard]] Result<u64, String_View> sync_time_if_stale_(i64 nm) noexcept {
        if(!time.should_refresh(nm, config.time_sync_max_age_ms)) {
            return Result<u64, String_View>::ok(0);
        }
        return time.refresh(session.tls, config.host);
    }
};

} // namespace spp::App::Okx
