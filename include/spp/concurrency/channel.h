#pragma once

#include <spp/concurrency/thread.h>
#include <spp/containers/queue.h>

namespace spp::Concurrency {

template<Move_Constructable T, Scalar_Allocator A>
struct Mpmc_Receiver;

template<Move_Constructable T, Scalar_Allocator A>
struct Mpmc_Sender;

template<Move_Constructable T, Scalar_Allocator A>
[[nodiscard]] Pair<Mpmc_Sender<T, A>, Mpmc_Receiver<T, A>> mpmc_channel(u64 capacity) noexcept;

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc>
struct Mpmc_State {
    explicit Mpmc_State(u64 cap) noexcept : queue(cap ? cap : 1) {
    }

    Thread::Mutex mutex;
    Thread::Cond not_empty;
    Thread::Cond not_full;
    Queue<T, A> queue;
    bool closed = false;
    u64 sender_count = 1;
    u64 receiver_count = 1;
};

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc>
struct Mpmc_Sender {
    using State = Mpmc_State<T, A>;

    Mpmc_Sender() noexcept = default;
    ~Mpmc_Sender() noexcept {
        drop();
    }

    Mpmc_Sender(const Mpmc_Sender&) noexcept = delete;
    Mpmc_Sender& operator=(const Mpmc_Sender&) noexcept = delete;

    Mpmc_Sender(Mpmc_Sender&& src) noexcept {
        state_ = spp::move(src.state_);
    }
    Mpmc_Sender& operator=(Mpmc_Sender&& src) noexcept {
        if(this == &src) return *this;
        drop();
        state_ = spp::move(src.state_);
        return *this;
    }

    [[nodiscard]] Mpmc_Sender dup() noexcept {
        Mpmc_Sender ret;
        if(!state_.ok()) return ret;
        {
            Thread::Lock lock{state_->mutex};
            state_->sender_count++;
        }
        ret.state_ = state_.dup();
        return ret;
    }

    [[nodiscard]] bool ok() const noexcept {
        return state_.ok();
    }

    [[nodiscard]] Result<u64, String_View> send(T&& value) noexcept {
        if(!state_.ok()) return Result<u64, String_View>::err("channel_closed"_v);

        Thread::Lock lock{state_->mutex};
        while(state_->queue.full() && !state_->closed && state_->receiver_count > 0) {
            state_->not_full.wait(state_->mutex);
        }
        if(state_->closed || state_->receiver_count == 0) {
            return Result<u64, String_View>::err("channel_closed"_v);
        }
        state_->queue.push(spp::move(value));
        state_->not_empty.signal();
        return Result<u64, String_View>::ok(1);
    }

    [[nodiscard]] Result<u64, String_View> send(const T& value) noexcept
        requires Copy_Constructable<T>
    {
        return send(T{value});
    }

    [[nodiscard]] bool try_send(T&& value) noexcept {
        if(!state_.ok()) return false;
        Thread::Lock lock{state_->mutex};
        if(state_->closed || state_->receiver_count == 0 || state_->queue.full()) {
            return false;
        }
        state_->queue.push(spp::move(value));
        state_->not_empty.signal();
        return true;
    }

    void close() noexcept {
        if(!state_.ok()) return;
        Thread::Lock lock{state_->mutex};
        state_->closed = true;
        state_->not_empty.broadcast();
        state_->not_full.broadcast();
    }

    void clear() noexcept {
        drop();
    }

private:
    explicit Mpmc_Sender(Arc<State, A>&& state) noexcept : state_(spp::move(state)) {
    }

    void drop() noexcept {
        if(!state_.ok()) return;
        {
            Thread::Lock lock{state_->mutex};
            if(state_->sender_count > 0) {
                state_->sender_count--;
                if(state_->sender_count == 0) {
                    state_->closed = true;
                    state_->not_empty.broadcast();
                    state_->not_full.broadcast();
                }
            }
        }
        state_.clear();
    }

    Arc<State, A> state_;

    template<Move_Constructable U, Scalar_Allocator B>
    friend struct Mpmc_Receiver;
    template<Move_Constructable U, Scalar_Allocator B>
    friend Pair<Mpmc_Sender<U, B>, Mpmc_Receiver<U, B>> mpmc_channel(u64 capacity) noexcept;
};

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc>
struct Mpmc_Receiver {
    using State = Mpmc_State<T, A>;

    Mpmc_Receiver() noexcept = default;
    ~Mpmc_Receiver() noexcept {
        drop();
    }

    Mpmc_Receiver(const Mpmc_Receiver&) noexcept = delete;
    Mpmc_Receiver& operator=(const Mpmc_Receiver&) noexcept = delete;

    Mpmc_Receiver(Mpmc_Receiver&& src) noexcept {
        state_ = spp::move(src.state_);
    }
    Mpmc_Receiver& operator=(Mpmc_Receiver&& src) noexcept {
        if(this == &src) return *this;
        drop();
        state_ = spp::move(src.state_);
        return *this;
    }

    [[nodiscard]] Mpmc_Receiver dup() noexcept {
        Mpmc_Receiver ret;
        if(!state_.ok()) return ret;
        {
            Thread::Lock lock{state_->mutex};
            state_->receiver_count++;
        }
        ret.state_ = state_.dup();
        return ret;
    }

    [[nodiscard]] bool ok() const noexcept {
        return state_.ok();
    }

    [[nodiscard]] Result<T, String_View> recv() noexcept {
        if(!state_.ok()) return Result<T, String_View>::err("channel_closed"_v);

        Thread::Lock lock{state_->mutex};
        while(state_->queue.empty() && !state_->closed && state_->sender_count > 0) {
            state_->not_empty.wait(state_->mutex);
        }
        if(state_->queue.empty()) {
            return Result<T, String_View>::err("channel_closed"_v);
        }

        T ret = spp::move(state_->queue.front());
        state_->queue.pop();
        state_->not_full.signal();
        return Result<T, String_View>::ok(spp::move(ret));
    }

    [[nodiscard]] Opt<T> try_recv() noexcept {
        if(!state_.ok()) return {};

        Thread::Lock lock{state_->mutex};
        if(state_->queue.empty()) return {};
        T ret = spp::move(state_->queue.front());
        state_->queue.pop();
        state_->not_full.signal();
        return Opt<T>{spp::move(ret)};
    }

    void close() noexcept {
        if(!state_.ok()) return;
        Thread::Lock lock{state_->mutex};
        state_->closed = true;
        state_->not_empty.broadcast();
        state_->not_full.broadcast();
    }

    void clear() noexcept {
        drop();
    }

private:
    explicit Mpmc_Receiver(Arc<State, A>&& state) noexcept : state_(spp::move(state)) {
    }

    void drop() noexcept {
        if(!state_.ok()) return;
        {
            Thread::Lock lock{state_->mutex};
            if(state_->receiver_count > 0) {
                state_->receiver_count--;
                if(state_->receiver_count == 0) {
                    state_->closed = true;
                    state_->not_empty.broadcast();
                    state_->not_full.broadcast();
                }
            }
        }
        state_.clear();
    }

    Arc<State, A> state_;

    template<Move_Constructable U, Scalar_Allocator B>
    friend Pair<Mpmc_Sender<U, B>, Mpmc_Receiver<U, B>> mpmc_channel(u64 capacity) noexcept;
};

template<Move_Constructable T, Scalar_Allocator A = Thread::Alloc>
[[nodiscard]] Pair<Mpmc_Sender<T, A>, Mpmc_Receiver<T, A>> mpmc_channel(u64 capacity) noexcept {
    using State = Mpmc_State<T, A>;
    Arc<State, A> state = Arc<State, A>::make(capacity);
    return Pair{Mpmc_Sender<T, A>{state.dup()}, Mpmc_Receiver<T, A>{spp::move(state)}};
}

} // namespace spp::Concurrency
