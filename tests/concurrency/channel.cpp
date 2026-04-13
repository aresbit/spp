#include "test.h"

#include <spp/concurrency/channel.h>

i32 main() {
    Test test{"empty"_v};

    Trace("Mpmc basic") {
        auto [sender, receiver] = Concurrency::mpmc_channel<i32>(1);
        assert(sender.send(7).ok());
        auto got = receiver.recv();
        assert(got.ok());
        assert(got.unwrap() == 7);

        auto none = receiver.try_recv();
        assert(!none.ok());
        auto none_result = receiver.try_recv_result();
        assert(!none_result.ok());
        assert(none_result.unwrap_err() == Concurrency::Channel_Error::empty);

        assert(sender.send(1).ok());
        auto full = sender.try_send_result(2);
        assert(!full.ok());
        assert(full.unwrap_err() == Concurrency::Channel_Error::full);
        auto one = receiver.recv();
        assert(one.ok() && one.unwrap() == 1);
    }

    Trace("Mpmc multi producer multi consumer") {
        constexpr i32 producers = 4;
        constexpr i32 consumers = 4;
        constexpr i32 per_producer = 1000;
        constexpr i64 expected_count = static_cast<i64>(producers) * per_producer;
        constexpr i64 expected_sum =
            static_cast<i64>(producers) * (static_cast<i64>(per_producer - 1) * per_producer / 2);

        auto [sender, receiver] = Concurrency::mpmc_channel<i32>(64);

        Vec<Thread::Future<void>> prod_tasks;
        for(i32 p = 0; p < producers; p++) {
            auto tx = sender.dup();
            prod_tasks.push(Thread::spawn([tx = spp::move(tx)]() mutable {
                for(i32 i = 0; i < per_producer; i++) {
                    auto sent = tx.send(i);
                    assert(sent.ok());
                }
            }));
        }
        sender.clear();

        Vec<Thread::Future<Pair<i64, i64>>> cons_tasks;
        for(i32 c = 0; c < consumers; c++) {
            auto rx = receiver.dup();
            cons_tasks.push(Thread::spawn([rx = spp::move(rx)]() mutable -> Pair<i64, i64> {
                i64 local_sum = 0;
                i64 local_count = 0;
                for(;;) {
                    auto got = rx.recv();
                    if(!got.ok()) break;
                    local_sum += got.unwrap();
                    local_count++;
                }
                return Pair{local_sum, local_count};
            }));
        }
        receiver.clear();

        for(auto& f : prod_tasks) {
            f->block();
        }

        i64 total_sum = 0;
        i64 total_count = 0;
        for(auto& f : cons_tasks) {
            auto part = f->block();
            total_sum += part.first;
            total_count += part.second;
        }

        assert(total_count == expected_count);
        assert(total_sum == expected_sum);
    }

    Trace("Mpmc close") {
        auto [sender, receiver] = Concurrency::mpmc_channel<i32>(2);
        sender.close();
        auto got = receiver.recv();
        assert(!got.ok());
        assert(got.unwrap_err() == Concurrency::Channel_Error::closed);
    }

    Trace("Mpmc disconnected") {
        auto [sender, receiver] = Concurrency::mpmc_channel<i32>(2);
        receiver.clear();
        auto sent = sender.send(1);
        assert(!sent.ok());
        assert(sent.unwrap_err() == Concurrency::Channel_Error::disconnected);
    }

    return 0;
}
