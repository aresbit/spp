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
            tasks.push(Thread::spawn([&cmap, t]() {
                for(i32 i = 0; i < per_thread; i++) {
                    i32 key = t * per_thread + i;
                    cmap.insert(spp::move(key), i);
                }
            }));
        }
        for(auto& task : tasks) task->block();

        assert(cmap.length() == static_cast<u64>(threads * per_thread));
        auto v = cmap.try_get_copy(777);
        assert(v.ok());
        assert(*v == 277);
        assert(cmap.erase(777));
        assert(!cmap.contains(777));
    }

    Trace("Concurrent vec push/pop") {
        Concurrency::Concurrent_Vec<i32> cvec;
        constexpr i32 threads = 4;
        constexpr i32 per_thread = 300;

        Vec<Thread::Future<void>> tasks;
        for(i32 t = 0; t < threads; t++) {
            tasks.push(Thread::spawn([&cvec, t]() {
                for(i32 i = 0; i < per_thread; i++) {
                    cvec.push(t * per_thread + i);
                }
            }));
        }
        for(auto& task : tasks) task->block();

        assert(cvec.length() == static_cast<u64>(threads * per_thread));

        u64 popped = 0;
        for(;;) {
            auto v = cvec.try_pop();
            if(!v.ok()) break;
            popped++;
        }
        assert(popped == static_cast<u64>(threads * per_thread));
        assert(cvec.length() == 0);
    }

    return 0;
}

