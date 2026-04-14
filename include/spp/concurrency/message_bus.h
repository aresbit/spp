#pragma once

#include <spp/concurrency/lockfree_ring.h>

namespace spp::Concurrency {

template<typename T, Allocator A = Mdefault>
struct Message_Bus {
    using Ring = Lockfree_Ring<T, A>;
    using Publish_Reservation = typename Ring::Write_Reservation;
    using Consume_Reservation = typename Ring::Read_Reservation;

    Message_Bus() noexcept = default;
    explicit Message_Bus(u64 capacity) noexcept : ring_(capacity) {
    }

    [[nodiscard]] bool valid() const noexcept {
        return ring_.valid();
    }

    [[nodiscard]] u64 capacity() const noexcept {
        return ring_.capacity();
    }

    [[nodiscard]] u64 approx_queued() const noexcept {
        return ring_.approx_size();
    }

    [[nodiscard]] bool try_publish(T&& msg) noexcept
        requires Move_Constructable<T>
    {
        return ring_.try_push(spp::move(msg));
    }

    [[nodiscard]] bool try_publish(const T& msg) noexcept
        requires Copy_Constructable<T>
    {
        return ring_.try_push(msg);
    }

    template<typename... Args>
        requires Constructable<T, Args...>
    [[nodiscard]] bool try_publish_emplace(Args&&... args) noexcept {
        auto res = try_reserve_publish();
        if(!res.ok()) return false;
        bool ok = res.emplace(spp::forward<Args>(args)...);
        assert(ok);
        return res.commit();
    }

    [[nodiscard]] Publish_Reservation try_reserve_publish() noexcept {
        return ring_.try_reserve_write();
    }

    [[nodiscard]] Consume_Reservation try_reserve_consume() noexcept {
        return ring_.try_reserve_read();
    }

    [[nodiscard]] Opt<T> try_recv() noexcept
        requires Move_Constructable<T>
    {
        return ring_.try_pop();
    }

    [[nodiscard]] Result<T, String_View> try_recv_result() noexcept
        requires Move_Constructable<T>
    {
        auto out = ring_.try_pop();
        if(!out.ok()) return Result<T, String_View>::err("empty"_v);
        return Result<T, String_View>::ok(spp::move(*out));
    }

private:
    Ring ring_;
};

} // namespace spp::Concurrency
