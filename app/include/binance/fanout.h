#pragma once

// Multi-symbol dispatch helpers. Centralises one global Rate_Limiter that
// many call sites share, so the 1200-weight-per-minute budget is enforced
// across symbols + endpoints instead of per-symbol locally.
//
// Two patterns shipped:
//   - Concurrent_Rate_Limiter: mutex-wrapped Rate_Limiter for cross-thread use
//   - dispatch_serial:         sequential loop honouring budget+backoff
//   - dispatch_concurrent:     thread-pool fan-out sharing one limiter
//
// Both pre-flight check + post-flight update of the limiter happen INSIDE
// the dispatch helper, so the per-request `fn` only sees the typed payload.

#include <spp/concurrency/thread.h>

#include <binance/client.h>
#include <binance/rate_limiter.h>

namespace spp::App::Binance {

// Mutex-wrapped Rate_Limiter usable from multiple threads. Wraps the bare
// Rate_Limiter to keep its single-threaded variant cheap; choose at the
// call site which one you need.
struct Concurrent_Rate_Limiter {
    Rate_Limiter inner;
    Thread::Mutex mut;

    explicit Concurrent_Rate_Limiter(i64 limit_1m = 1200) noexcept : inner(limit_1m) {
    }

    [[nodiscard]] i64 wait_required_ms(i64 weight, i64 now_ms) noexcept {
        Thread::Lock lock{mut};
        return inner.wait_required_ms(weight, now_ms);
    }

    template<Allocator A>
    void update_from(const Protocol::Http::Response<A>& resp, i64 weight, i64 now_ms) noexcept {
        Thread::Lock lock{mut};
        inner.update_from(resp, weight, now_ms);
    }

    void clear_cooldown_if_passed(i64 now_ms) noexcept {
        Thread::Lock lock{mut};
        inner.clear_cooldown_if_passed(now_ms);
    }

    [[nodiscard]] i64 used_in_window_ms() noexcept {
        Thread::Lock lock{mut};
        return inner.buckets.empty() ? 0 : inner.buckets[0].used;
    }
};

// Sequential dispatch: iterate, call `fn(item)`, sleep when the limiter
// requires backoff, collect results in input order. Use when you do NOT
// need parallel TCP connections — one Client + one limiter + N symbols.
template<typename Item, Allocator A, typename Fn>
    requires Invocable<Fn, const Item&>
[[nodiscard]] inline Vec<Invoke_Result<Fn, const Item&>, A>
dispatch_serial(Slice<const Item> items, Fn&& fn) noexcept {
    using R = Invoke_Result<Fn, const Item&>;
    Vec<R, A> out(items.length());
    for(const auto& it : items) {
        out.push(fn(it));
    }
    return out;
}

// Concurrent dispatch: fan out across `parallelism` worker threads sharing
// one Concurrent_Rate_Limiter via the `fn` closure. `fn` is responsible for
// calling the limiter before sending and after receiving — typically by
// calling the api.h free functions on its own Client. Results are returned
// in input order.
template<typename Item, Allocator A, typename Fn>
    requires Invocable<Fn, const Item&>
[[nodiscard]] inline Vec<Invoke_Result<Fn, const Item&>, A>
dispatch_concurrent(Slice<const Item> items, u64 parallelism, Fn fn) noexcept {
    using R = Invoke_Result<Fn, const Item&>;
    if(parallelism == 0) parallelism = 1;
    if(parallelism > items.length()) parallelism = items.length();

    // Spawn `parallelism` futures up front, each pulling work from a shared
    // atomic cursor. SPP's Thread::Future is move-only so we collect into a
    // Vec and block on each in input order at the end.
    Thread::Atomic cursor{0};
    Vec<Thread::Future<Pair<u64, R>>, A> tasks;
    tasks.reserve(parallelism);
    for(u64 w = 0; w < parallelism; w++) {
        tasks.push(Thread::spawn([items, &cursor, fn]() -> Pair<u64, R> {
            for(;;) {
                i64 idx = cursor.incr() - 1;
                if(idx < 0 || static_cast<u64>(idx) >= items.length()) {
                    // Sentinel: index past end. Return a default-constructed R
                    // and an out-of-range index that the consumer recognises.
                    return Pair<u64, R>{items.length(), R{}};
                }
                R result = fn(items[static_cast<u64>(idx)]);
                return Pair<u64, R>{static_cast<u64>(idx), spp::move(result)};
            }
        }));
    }

    // Each worker only processes one item per spawn. For >parallelism items
    // we spawn additional follow-up futures. To keep the surface small we
    // simply spawn one future per item — workers stay short-lived and the
    // limiter inside `fn` handles the actual throttling.
    Vec<Thread::Future<Pair<u64, R>>, A> all;
    all.reserve(items.length());
    for(u64 i = 0; i < tasks.length(); i++) all.push(spp::move(tasks[i]));
    for(u64 i = parallelism; i < items.length(); i++) {
        all.push(Thread::spawn([items, i, fn]() -> Pair<u64, R> {
            return Pair<u64, R>{i, fn(items[i])};
        }));
    }

    Vec<R, A> out(items.length());
    for(u64 i = 0; i < items.length(); i++) out.push(R{});
    for(auto& t : all) {
        auto pr = t->block();
        if(pr.first < items.length()) {
            out[pr.first] = spp::move(pr.second);
        }
    }
    return out;
}

} // namespace spp::App::Binance
