#pragma once

#include <spp/async/pool.h>
#include <spp/concurrency/channel.h>

namespace spp::Async {

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

} // namespace spp::Async
