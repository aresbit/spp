#pragma once

#include <spp/async/pool.h>
#include <spp/async/asyncio.h>
#include <spp/concurrency/channel.h>

namespace spp::Async {

[[nodiscard]] inline Concurrency::Channel_Error
map_wait_error_to_channel(String_View err) noexcept {
    if(err == "cancelled"_v) return Concurrency::Channel_Error::cancelled;
    return Concurrency::Channel_Error::timeout;
}

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc, Allocator P = Alloc>
[[nodiscard]] Task<Result<T, Concurrency::Channel_Error>, P> recv(Pool<P>& pool,
                                                                  Concurrency::Mpmc_Receiver<T, A>& rx) noexcept {
    for(;;) {
        auto got = rx.try_recv_result();
        if(got.ok()) {
            co_return Result<T, Concurrency::Channel_Error>::ok(spp::move(got.unwrap()));
        }

        auto err = got.unwrap_err();
        if(err == Concurrency::Channel_Error::closed ||
           err == Concurrency::Channel_Error::disconnected) {
            co_return Result<T, Concurrency::Channel_Error>::err(spp::move(err));
        }

        co_await pool.suspend();
    }
}

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc, Allocator P = Alloc>
[[nodiscard]] Task<Result<u64, Concurrency::Channel_Error>, P> send(
    Pool<P>& pool, Concurrency::Mpmc_Sender<T, A>& tx, T&& value) noexcept {

    for(;;) {
        auto sent = tx.try_send_result(spp::move(value));
        if(sent.ok()) {
            co_return Result<u64, Concurrency::Channel_Error>::ok(u64{sent.unwrap()});
        }

        auto err = sent.unwrap_err();
        if(err == Concurrency::Channel_Error::closed ||
           err == Concurrency::Channel_Error::disconnected) {
            co_return Result<u64, Concurrency::Channel_Error>::err(spp::move(err));
        }

        co_await pool.suspend();
    }
}

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc, Allocator P = Alloc>
[[nodiscard]] Task<Result<T, Concurrency::Channel_Error>, P> recv_for(
    Pool<P>& pool, Concurrency::Mpmc_Receiver<T, A>& rx, u64 timeout_ms,
    const Cancel_Token& token) noexcept {

    u64 start = Thread::perf_counter();
    u64 freq = Thread::perf_frequency();

    for(;;) {
        if(token.cancelled()) {
            co_return Result<T, Concurrency::Channel_Error>::err(Concurrency::Channel_Error::cancelled);
        }

        auto got = rx.try_recv_result();
        if(got.ok()) {
            co_return Result<T, Concurrency::Channel_Error>::ok(spp::move(got.unwrap()));
        }

        auto err = got.unwrap_err();
        if(err == Concurrency::Channel_Error::closed ||
           err == Concurrency::Channel_Error::disconnected) {
            co_return Result<T, Concurrency::Channel_Error>::err(spp::move(err));
        }

        u64 elapsed_ms = (Thread::perf_counter() - start) * 1000 / freq;
        if(elapsed_ms >= timeout_ms) {
            co_return Result<T, Concurrency::Channel_Error>::err(Concurrency::Channel_Error::timeout);
        }

        u64 remain_ms = timeout_ms - elapsed_ms;
        u64 step_ms = remain_ms > 1 ? 1 : remain_ms;
        auto waited = co_await wait_result(pool, step_ms, token);
        if(!waited.ok()) {
            co_return Result<T, Concurrency::Channel_Error>::err(
                map_wait_error_to_channel(waited.unwrap_err()));
        }
    }
}

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc, Allocator P = Alloc>
[[nodiscard]] Task<Result<T, Concurrency::Channel_Error>, P> recv_for(
    Pool<P>& pool, Concurrency::Mpmc_Receiver<T, A>& rx, u64 timeout_ms) noexcept {
    Cancel_Token token;
    co_return co_await recv_for(pool, rx, timeout_ms, token);
}

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc, Allocator P = Alloc>
[[nodiscard]] Task<Result<u64, Concurrency::Channel_Error>, P> send_for(
    Pool<P>& pool, Concurrency::Mpmc_Sender<T, A>& tx, T value, u64 timeout_ms,
    const Cancel_Token& token) noexcept {

    u64 start = Thread::perf_counter();
    u64 freq = Thread::perf_frequency();

    for(;;) {
        if(token.cancelled()) {
            co_return Result<u64, Concurrency::Channel_Error>::err(
                Concurrency::Channel_Error::cancelled);
        }

        auto sent = tx.try_send_result(spp::move(value));
        if(sent.ok()) {
            co_return Result<u64, Concurrency::Channel_Error>::ok(u64{sent.unwrap()});
        }

        auto err = sent.unwrap_err();
        if(err == Concurrency::Channel_Error::closed ||
           err == Concurrency::Channel_Error::disconnected) {
            co_return Result<u64, Concurrency::Channel_Error>::err(spp::move(err));
        }

        u64 elapsed_ms = (Thread::perf_counter() - start) * 1000 / freq;
        if(elapsed_ms >= timeout_ms) {
            co_return Result<u64, Concurrency::Channel_Error>::err(Concurrency::Channel_Error::timeout);
        }

        u64 remain_ms = timeout_ms - elapsed_ms;
        u64 step_ms = remain_ms > 1 ? 1 : remain_ms;
        auto waited = co_await wait_result(pool, step_ms, token);
        if(!waited.ok()) {
            co_return Result<u64, Concurrency::Channel_Error>::err(
                map_wait_error_to_channel(waited.unwrap_err()));
        }
    }
}

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc, Allocator P = Alloc>
[[nodiscard]] Task<Result<u64, Concurrency::Channel_Error>, P> send_for(
    Pool<P>& pool, Concurrency::Mpmc_Sender<T, A>& tx, T value, u64 timeout_ms) noexcept {
    Cancel_Token token;
    co_return co_await send_for(pool, tx, spp::move(value), timeout_ms, token);
}

} // namespace spp::Async
