#pragma once

// Async wrappers over the blocking Client facade. Each request is dispatched
// to a worker thread via Thread::spawn and returns a Thread::Future. This is
// the pragmatic-today path: the underlying TLS + HTTP layer is still
// blocking, but call-sites can fan out concurrent requests without each one
// stalling the caller.
//
// Upgrade path: when SPP's async runtime grows non-blocking sockets +
// mbedTLS WANT_READ/WRITE state-machine integration, this wrapper will swap
// `Thread::spawn` for `Async::Task` and the public API stays the same.
//
// IMPORTANT: each Future runs against the SAME Client object. Concurrent
// access to one Client serialises through its internal Tls_Mbedtls_Stream
// (which is not thread-safe). For parallel pipelines, create one Client per
// worker, or wrap the Client itself in a mutex; this header just sketches
// the off-thread-dispatch surface.

#include <spp/concurrency/thread.h>

#include <binance/client.h>

namespace spp::App::Binance {

struct Async_Client {
    Client& inner;

    explicit Async_Client(Client& c) noexcept : inner(c) {
    }

    [[nodiscard]] Thread::Future<Result<Server_Time, String_View>>
    server_time() noexcept {
        return Thread::spawn([this]() { return inner.server_time(); });
    }

    [[nodiscard]] Thread::Future<Result<Ticker_Price, String_View>>
    ticker_price(String_View symbol) noexcept {
        auto symbol_owned = symbol.string<Mdefault>();
        return Thread::spawn(
            [this, sym = spp::move(symbol_owned)]() mutable {
                return inner.ticker_price(sym.view());
            });
    }

    [[nodiscard]] Thread::Future<Result<Account_Info, String_View>> account() noexcept {
        return Thread::spawn([this]() { return inner.account(); });
    }

    [[nodiscard]] Thread::Future<Result<Order_Response, String_View>>
    place_order(Order_Request order) noexcept {
        // Capture by value to be safe across thread boundary.
        return Thread::spawn(
            [this, req = spp::move(order)]() mutable { return inner.place_order(req); });
    }
};

} // namespace spp::App::Binance
