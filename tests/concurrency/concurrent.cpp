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

    Trace("Concurrent map composite ops") {
        Concurrency::Concurrent_Map<i32, i32> cmap;
        assert(cmap.get_or_insert_with(1, [] { return 5; }));
        assert(!cmap.get_or_insert_with(1, [] { return 99; }));
        auto value1 = cmap.try_get_copy(1);
        assert(value1.ok() && *value1 == 5);

        assert(!cmap.upsert(1, [] { return 10; }, [](i32& x) { x += 3; }));
        auto v = cmap.try_get_copy(1);
        assert(v.ok() && *v == 8);
        assert(cmap.upsert(2, [] { return 7; }, [](i32& x) { x += 10; }));
        auto v2 = cmap.try_get_copy(2);
        assert(v2.ok() && *v2 == 7);

        assert(cmap.update_if(2, [](i32& x) { x *= 2; }));
        auto got2 = cmap.try_get_copy(2);
        assert(got2.ok() && *got2 == 14);

        assert(!cmap.erase_if(2, [](const i32& x) { return x < 10; }));
        assert(cmap.erase_if(2, [](const i32& x) { return x == 14; }));
        assert(!cmap.contains(2));
    }

    Trace("Concurrent map upsert contention") {
        Concurrency::Concurrent_Map<i32, i32> cmap;
        constexpr i32 threads = 8;
        constexpr i32 iters = 3000;

        Vec<Thread::Future<void>> tasks;
        for(i32 t = 0; t < threads; t++) {
            tasks.push(Thread::spawn([&cmap]() mutable {
                for(i32 i = 0; i < iters; i++) {
                    (void)cmap.upsert(7, [] { return 1; }, [](i32& x) { x += 1; });
                }
            }));
        }
        for(auto& task : tasks) task->block();

        auto got = cmap.try_get_copy(7);
        assert(got.ok());
        assert(*got == threads * iters);
    }

    Trace("Concurrent map snapshot and drain") {
        Concurrency::Concurrent_Map<i32, i32> cmap;
        cmap.batch_write([](auto& map) {
            for(i32 i = 0; i < 32; i++) map.insert(i, i * 2);
        });
        auto snap = cmap.snapshot();
        assert(snap.length() == 32);
        auto drained = cmap.drain_all();
        assert(drained.length() == 32);
        assert(cmap.length() == 0);
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

    Trace("Concurrent vec snapshot and drain") {
        Concurrency::Concurrent_Vec<i32> cvec;
        cvec.batch_write([](auto& vec) {
            for(i32 i = 0; i < 64; i++) vec.push(i);
        });
        auto snap = cvec.snapshot();
        assert(snap.length() == 64);
        auto drained = cvec.drain_all();
        assert(drained.length() == 64);
        assert(cvec.length() == 0);
    }

    return 0;
}
