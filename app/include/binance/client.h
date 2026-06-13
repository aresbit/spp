#pragma once

// Production facade. Requires SPP_TLS=1 — pulls in mbedTLS.
//
// Combines:
//   - Net::Tcp_Client / Ext::Tls_Mbedtls_Stream  (transport, TLS-terminated)
//   - Time_Sync                                   (clock skew vs serverTime)
//   - Rate_Limiter                                (weight accounting + 429/418)
//
// Methods delegate to api.h free functions (which are Memory_Stream-friendly),
// so every line of business logic is covered by the same unit tests.

#include <spp/ext/tls_mbedtls.h>

#include <binance/api.h>
#include <binance/clock.h>
#include <binance/models.h>
#include <binance/rate_limiter.h>
#include <binance/time_sync.h>

namespace spp::App::Binance {

struct Client_Config {
    String_View host = "api.binance.com"_v;
    u16 port = 443;
    String_View api_key;
    String_View api_secret;
    i64 recv_window_ms = 5000;
    bool insecure_skip_verify = false; // testnet self-signed shims only
    i64 time_sync_max_age_ms = 60000;
};

struct Client {
    explicit Client(Client_Config cfg) noexcept : config(spp::move(cfg)) {
    }

    Client_Config config;
    Ext::Tls_Mbedtls_Stream stream;
    Time_Sync time;
    Rate_Limiter limiter;

    [[nodiscard]] Result<u64, String_View> connect() noexcept {
        Ext::Tls_Mbedtls_Stream::Options opts;
        opts.insecure_skip_verify = config.insecure_skip_verify;
        return stream.connect_result(config.host, config.port, opts);
    }

    void close() noexcept {
        stream.close();
    }

    // Resync if the cached skew is stale or this is the first call.
    [[nodiscard]] Result<u64, String_View> sync_time_if_stale() noexcept {
        i64 nm = now_ms();
        if(!time.should_refresh(nm, config.time_sync_max_age_ms)) {
            return Result<u64, String_View>::ok(0);
        }
        return time.refresh(stream, config.host);
    }

    [[nodiscard]] Result<Server_Time, String_View> server_time() noexcept {
        return fetch_server_time(stream, config.host, limiter, now_ms());
    }

    [[nodiscard]] Result<Ticker_Price, String_View> ticker_price(String_View symbol) noexcept {
        return fetch_ticker_price(stream, config.host, limiter, now_ms(), symbol);
    }

    [[nodiscard]] Result<Account_Info, String_View> account() noexcept {
        auto synced = sync_time_if_stale();
        if(!synced.ok()) return Result<Account_Info, String_View>::err(spp::move(synced.unwrap_err()));
        return fetch_account(stream, config.host, config.api_key, config.api_secret, limiter,
                             time, now_ms(), config.recv_window_ms);
    }

    [[nodiscard]] Result<Order_Response, String_View>
    place_order(const Order_Request& order) noexcept {
        auto synced = sync_time_if_stale();
        if(!synced.ok())
            return Result<Order_Response, String_View>::err(spp::move(synced.unwrap_err()));
        return Binance::place_order(stream, config.host, config.api_key, config.api_secret,
                                     limiter, time, now_ms(), order, config.recv_window_ms);
    }

    [[nodiscard]] Result<Vec<Kline, Mdefault>, String_View>
    klines(String_View symbol, String_View interval, u64 limit = 500,
           Opt<i64> start_ms = {}, Opt<i64> end_ms = {}) noexcept {
        return fetch_klines(stream, config.host, limiter, now_ms(), symbol, interval,
                            limit, start_ms, end_ms);
    }

    [[nodiscard]] Result<Order_State, String_View>
    query_order(String_View symbol, i64 order_id) noexcept {
        auto synced = sync_time_if_stale();
        if(!synced.ok())
            return Result<Order_State, String_View>::err(spp::move(synced.unwrap_err()));
        return Binance::query_order(stream, config.host, config.api_key, config.api_secret,
                                     limiter, time, now_ms(), symbol, order_id,
                                     config.recv_window_ms);
    }

    [[nodiscard]] Result<Order_State, String_View>
    cancel_order(String_View symbol, i64 order_id) noexcept {
        auto synced = sync_time_if_stale();
        if(!synced.ok())
            return Result<Order_State, String_View>::err(spp::move(synced.unwrap_err()));
        return Binance::cancel_order(stream, config.host, config.api_key, config.api_secret,
                                      limiter, time, now_ms(), symbol, order_id,
                                      config.recv_window_ms);
    }

    [[nodiscard]] Result<Book_Ticker, String_View>
    book_ticker(String_View symbol) noexcept {
        return fetch_book_ticker(stream, config.host, limiter, now_ms(), symbol);
    }
};

} // namespace spp::App::Binance
