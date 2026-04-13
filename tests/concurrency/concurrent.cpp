#include "test.h"

#include <spp/concurrency/concurrent.h>

i32 main() {
    Test test{"empty"_v};

    Trace("Concurrent map insert/lookup") {
        Concurrency::Concurrent_Map<i32, i32> cmap;

        constexpr i32 threads = 4;
        constexpr i32 per_thread = 500;

        Vec<Thread::Future<void>> tasks;
        for(i32 t = 0; t < threads; t++) {
            tasks.push(Thread::spawn([&cmap, t]() mutable {
                for(i32 i = 0; i < per_thread; i++) {
                    i32 key = t * per_thread + i;
                    cmap.write([&](auto& map) { map.insert(spp::move(key), i); });
                }
            }));
        }
        for(auto& task : tasks) task->block();

        auto total = cmap.read([](const auto& map) { return map.length(); });
        assert(total == static_cast<u64>(threads * per_thread));
        auto v = cmap.with_lock([](const auto& map) { return map.try_get(777); });
        assert(v.ok());
        assert(**v == 277);
        assert(cmap.write([](auto& map) { return map.try_erase(777); }));
        assert(!cmap.contains(777));
    }

    Trace("Concurrent vec push/pop") {
        Concurrency::Concurrent_Vec<i32> cvec;
        constexpr i32 threads = 4;
        constexpr i32 per_thread = 300;

        Vec<Thread::Future<void>> tasks;
        for(i32 t = 0; t < threads; t++) {
            tasks.push(Thread::spawn([&cvec, t]() mutable {
                for(i32 i = 0; i < per_thread; i++) {
                    cvec.write([&](auto& vec) { vec.push(t * per_thread + i); });
                }
            }));
        }
        for(auto& task : tasks) task->block();

        auto len = cvec.read([](const auto& vec) { return vec.length(); });
        assert(len == static_cast<u64>(threads * per_thread));

        u64 popped = 0;
        for(;;) {
            auto v = cvec.write([](auto& vec) -> Opt<i32> {
                if(vec.empty()) return {};
                i32 out = spp::move(vec.back());
                vec.pop();
                return Opt<i32>{spp::move(out)};
            });
            if(!v.ok()) break;
            popped++;
        }
        assert(popped == static_cast<u64>(threads * per_thread));
        assert(cvec.length() == 0);
    }

    return 0;
}
