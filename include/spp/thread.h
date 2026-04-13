
#pragma once

#include "base.h"
#include "rc.h"

namespace spp {
namespace Thread {

#ifdef SPP_OS_WINDOWS
using OS_Thread = void*;
using OS_Thread_Ret = u32;
constexpr OS_Thread OS_Thread_Null = null;
constexpr OS_Thread_Ret OS_Thread_Ret_Null = 0;
#else
using OS_Thread = pthread_t;
using OS_Thread_Ret = void*;
constexpr OS_Thread OS_Thread_Null = 0;
constexpr OS_Thread_Ret OS_Thread_Ret_Null = null;
#endif

[[nodiscard]] Id sys_id(OS_Thread thread) noexcept;
void sys_join(OS_Thread thread) noexcept;
void sys_detach(OS_Thread thread) noexcept;
[[nodiscard]] OS_Thread sys_start(OS_Thread_Ret (*f)(void*), void* data) noexcept;

template<typename T>
struct Promise {

    Promise() noexcept = default;
    ~Promise() noexcept = default;

    Promise(const Promise&) noexcept = delete;
    Promise(Promise&&) noexcept = delete;
    Promise& operator=(const Promise&) noexcept = delete;
    Promise& operator=(Promise&&) noexcept = delete;

    void fill(T&& val) noexcept {
        value = spp::move(val);
        flag.signal();
    }

    [[nodiscard]] T block() noexcept {
        flag.block();
        return spp::move(value);
    }

    [[nodiscard]] bool ready() noexcept {
        return flag.ready();
    }

private:
    Flag flag;
    T value;

    friend struct Reflect::Refl<Promise<T>>;
};

template<>
struct Promise<void> {

    Promise() noexcept = default;
    ~Promise() noexcept = default;

    Promise(const Promise&) noexcept = delete;
    Promise(Promise&&) noexcept = delete;
    Promise& operator=(const Promise&) noexcept = delete;
    Promise& operator=(Promise&&) noexcept = delete;

    void fill() noexcept {
        flag.signal();
    }

    void block() noexcept {
        flag.block();
    }

    [[nodiscard]] bool ready() noexcept {
        return flag.ready();
    }

private:
    Flag flag;

    friend struct Reflect::Refl<Promise<void>>;
};

template<typename T, Scalar_Allocator A = Alloc>
using Future = Arc<Promise<T>, A>;

template<Allocator A = Alloc>
struct Thread {

    Thread() noexcept = default;

    template<Invocable F>
    explicit Thread(F&& f) noexcept {
        F* data = reinterpret_cast<F*>(A::alloc(sizeof(F)));
        new(data) F{spp::forward<F>(f)};
        thread = sys_start(&invoke<F>, data);
    }
    ~Thread() noexcept {
        if(thread) join();
    }

    Thread(const Thread&) noexcept = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&& src) noexcept {
        thread = src.thread;
        src.thread = OS_Thread_Null;
    }
    Thread& operator=(Thread&& src) noexcept {
        this->~Thread();
        thread = src.thread;
        src.thread = OS_Thread_Null;
        return *this;
    }

    [[nodiscard]] Id id() noexcept {
        assert(thread);
        return sys_id(thread);
    }
    void join() noexcept {
        assert(thread);
        sys_join(thread);
        thread = OS_Thread_Null;
    }
    void detach() noexcept {
        assert(thread);
        sys_detach(thread);
        thread = OS_Thread_Null;
    }

private:
    OS_Thread thread = OS_Thread_Null;

    template<Invocable F>
    [[nodiscard]] static OS_Thread_Ret invoke(void* _f) noexcept {
        F* f = static_cast<F*>(_f);
        (*f)();
        f->~F();
        A::free(f);
        return OS_Thread_Ret_Null;
    }

    friend struct Reflect::Refl<Thread<A>>;
};

template<Allocator A = Alloc, typename F, typename... Args>
    requires Invocable<F, Args...>
[[nodiscard]] auto spawn(F&& f, Args&&... args) noexcept -> Future<Invoke_Result<F, Args...>, A> {

    using Result = Invoke_Result<F, Args...>;
    auto future = Future<Result, A>::make();

    Thread thread{[future = future.dup(), f = spp::forward<F>(f),
                   ... args = spp::forward<Args>(args)]() mutable {
        if constexpr(Same<Result, void>) {
            f(spp::forward<Args>(args)...);
            future->fill();
        } else {
            future->fill(f(spp::forward<Args>(args)...));
        }
    }};
    thread.detach();

    return future;
}

} // namespace Thread

template<typename T>
SPP_NAMED_TEMPLATE_RECORD(::spp::Thread::Promise, "Promise", T, SPP_FIELD(flag), SPP_FIELD(value));

template<>
SPP_NAMED_TEMPLATE_RECORD(::spp::Thread::Promise, "Promise", void, SPP_FIELD(flag));

template<Allocator A>
SPP_NAMED_TEMPLATE_RECORD(::spp::Thread::Thread, "Thread", A, SPP_FIELD(thread));

} // namespace spp
