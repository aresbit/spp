#include "test.h"

#include <binance/fanout.h>

namespace Bnc = spp::App::Binance;

i32 main() {
    Test test{"empty"_v};

    Trace("Concurrent_Rate_Limiter forwards single-threaded calls") {
        Bnc::Concurrent_Rate_Limiter rl(1200);
        // Pre-flight: budget is fresh, should permit immediately.
        assert(rl.wait_required_ms(10, 0) == 0);
        // No traffic yet → used is 0.
        assert(rl.used_in_window_ms() == 0);
    }

    Trace("Concurrent_Rate_Limiter safe under multi-thread access") {
        Bnc::Concurrent_Rate_Limiter rl(1200);
        // Spawn 8 threads each doing a tiny burst against the same limiter.
        // We assert no crashes and the pre-flight check converges.
        spp::Vec<spp::Thread::Future<spp::i64>, Mdefault> tasks;
        tasks.reserve(8);
        for(int w = 0; w < 8; w++) {
            tasks.push(spp::Thread::spawn([&rl]() -> spp::i64 {
                spp::i64 total = 0;
                for(int i = 0; i < 50; i++) {
                    total += rl.wait_required_ms(1, 0);
                }
                return total;
            }));
        }
        spp::i64 grand_total = 0;
        for(auto& t : tasks) grand_total += t->block();
        // Under no rate-limit pressure, every wait should be 0.
        assert(grand_total == 0);
    }

    Trace("dispatch_serial preserves input order, calls fn per item") {
        spp::Vec<spp::String_View, Mdefault> symbols;
        symbols.push("BTCUSDT"_v);
        symbols.push("ETHUSDT"_v);
        symbols.push("BNBUSDT"_v);

        spp::Thread::Atomic call_count{0};
        auto results = Bnc::dispatch_serial<spp::String_View, Mdefault>(
            symbols.slice(),
            [&call_count](const spp::String_View& s) -> spp::String<Mdefault> {
                call_count.incr();
                return s.append<Mdefault>("!"_v);
            });
        assert(call_count.load() == 3);
        assert(results.length() == 3);
        assert(results[0] == "BTCUSDT!"_v);
        assert(results[1] == "ETHUSDT!"_v);
        assert(results[2] == "BNBUSDT!"_v);
    }

    Trace("dispatch_concurrent runs N items across workers, preserves order") {
        spp::Vec<spp::i64, Mdefault> inputs;
        for(spp::i64 i = 0; i < 20; i++) inputs.push(i);

        auto results = Bnc::dispatch_concurrent<spp::i64, Mdefault>(
            inputs.slice(), 4,
            [](const spp::i64& v) -> spp::i64 { return v * 10; });
        assert(results.length() == 20);
        for(spp::u64 i = 0; i < 20; i++) {
            assert(results[i] == static_cast<spp::i64>(i) * 10);
        }
    }

    return 0;
}
