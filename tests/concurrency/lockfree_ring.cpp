#include "test.h"

i32 main() {
    Test test{"empty"_v};

    Trace("Lockfree ring basic") {
        Concurrency::Lockfree_Ring<i32> ring(8);
        assert(ring.valid());
        assert(ring.capacity() == 8);
        assert(ring.approx_size() == 0);

        assert(ring.try_push(1));
        assert(ring.try_push(2));
        assert(ring.approx_size() == 2);

        auto v1 = ring.try_pop();
        auto v2 = ring.try_pop();
        assert(v1.ok() && *v1 == 1);
        assert(v2.ok() && *v2 == 2);
        assert(!ring.try_pop().ok());
    }

    Trace("Message bus zero-copy reserve/commit") {
        struct Tick {
            i64 id;
            i64 px;
            i64 qty;
        };

        Concurrency::Message_Bus<Tick> bus(4);
        assert(bus.valid());

        {
            auto pub = bus.try_reserve_publish();
            assert(pub.ok());
            assert(pub.emplace(7, 123456, 9));
            assert(pub.commit());
        }

        {
            auto sub = bus.try_reserve_consume();
            assert(sub.ok());
            auto& tick = sub.value();
            assert(tick.id == 7);
            assert(tick.px == 123456);
            assert(tick.qty == 9);
            sub.commit();
        }

        {
            auto pub = bus.try_reserve_publish();
            assert(pub.ok());
            assert(pub.emplace(8, 1, 2));
            pub.cancel();
            assert(!bus.try_reserve_consume().ok());
        }
    }

    Trace("Lockfree ring MPMC") {
        constexpr i32 producers = 4;
        constexpr i32 consumers = 4;
        constexpr i32 per_producer = 10000;
        constexpr i64 total_count = static_cast<i64>(producers) * per_producer;
        constexpr i64 expected_sum = (total_count - 1) * total_count / 2;

        Concurrency::Message_Bus<i64> bus(1024);
        Thread::Atomic done_producers{0};

        Vec<Thread::Future<void>> prod_tasks;
        for(i32 p = 0; p < producers; p++) {
            prod_tasks.push(Thread::spawn([&bus, &done_producers, p]() mutable {
                i64 base = static_cast<i64>(p) * per_producer;
                for(i32 i = 0; i < per_producer; i++) {
                    i64 value = base + i;
                    while(!bus.try_publish(value)) {
                        Thread::pause();
                    }
                }
                done_producers.incr();
            }));
        }

        Vec<Thread::Future<Pair<i64, i64>>> cons_tasks;
        for(i32 c = 0; c < consumers; c++) {
            cons_tasks.push(Thread::spawn([&bus, &done_producers]() mutable -> Pair<i64, i64> {
                i64 local_sum = 0;
                i64 local_count = 0;
                for(;;) {
                    auto got = bus.try_recv();
                    if(got.ok()) {
                        local_sum += *got;
                        local_count++;
                        continue;
                    }
                    if(done_producers.load() == producers && bus.approx_queued() == 0) {
                        break;
                    }
                    Thread::pause();
                }
                return Pair{local_sum, local_count};
            }));
        }

        for(auto& task : prod_tasks) {
            task->block();
        }

        i64 total_sum = 0;
        i64 total_recv = 0;
        for(auto& task : cons_tasks) {
            auto part = task->block();
            total_sum += part.first;
            total_recv += part.second;
        }

        assert(total_recv == total_count);
        assert(total_sum == expected_sum);
    }

    return 0;
}
