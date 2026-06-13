#include "test.h"

#include <binance/async_client.h>

namespace Bnc = spp::App::Binance;

// Async_Client just dispatches blocking Client methods onto worker threads.
// We can't run a real Client in unit tests (it needs a TLS transport), but
// we can verify the dispatch mechanism in isolation by spawning lots of
// Thread::Future-returning lambdas and checking they all complete.

i32 main() {
    Test test{"empty"_v};

    Trace("Thread::spawn-style dispatch yields independent futures") {
        // Mirrors the shape of Async_Client without needing a Client.
        auto compute = [](spp::i64 x) -> spp::Thread::Future<spp::i64> {
            return spp::Thread::spawn([x]() -> spp::i64 { return x * x; });
        };

        spp::Vec<spp::Thread::Future<spp::i64>, Mdefault> tasks;
        for(spp::i64 i = 0; i < 12; i++) tasks.push(compute(i));

        spp::i64 sum = 0;
        for(spp::u64 i = 0; i < tasks.length(); i++) sum += tasks[i]->block();
        // 0^2 + 1^2 + ... + 11^2 = 506
        assert(sum == 506);
    }

    Trace("Result<T,E>-carrying futures preserve both branches") {
        using Maybe = spp::Result<spp::i64, spp::String_View>;
        auto fut_ok = spp::Thread::spawn([]() -> Maybe { return Maybe::ok(42); });
        auto fut_err = spp::Thread::spawn(
            []() -> Maybe { return Maybe::err("nope"_v); });
        auto r_ok = fut_ok->block();
        auto r_err = fut_err->block();
        assert(r_ok.ok());
        assert(r_ok.unwrap() == 42);
        assert(!r_err.ok());
        assert(r_err.unwrap_err() == "nope"_v);
    }

    return 0;
}
