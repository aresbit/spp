#include "test.h"

#include <spp/async/channel.h>

i32 main() {
    Test test{"empty"_v};

    Async::Pool pool;

    Trace("Async recv") {
        auto [tx, rx] = Concurrency::mpmc_channel<i32>(1);

        auto consumer = [&]() -> Async::Task<i32> {
            auto got = co_await Async::recv(pool, rx);
            assert(got.ok());
            co_return got.unwrap();
        }();

        auto producer = Thread::spawn([tx = spp::move(tx)]() mutable {
            Thread::sleep(10);
            auto sent = tx.send(42);
            assert(sent.ok());
        });

        assert(consumer.block() == 42);
        producer->block();
    }

    Trace("Async send") {
        auto [tx, rx] = Concurrency::mpmc_channel<i32>(1);
        assert(tx.send(1).ok());

        auto push = [&]() -> Async::Task<void> {
            auto sent = co_await Async::send(pool, tx, 2);
            assert(sent.ok());
            co_return;
        }();

        auto consumer = Thread::spawn([&rx]() {
            Thread::sleep(10);
            auto a = rx.recv();
            assert(a.ok() && a.unwrap() == 1);
            auto b = rx.recv();
            assert(b.ok() && b.unwrap() == 2);
        });

        push.block();
        consumer->block();
    }

    Trace("Async channel error") {
        auto [tx, rx] = Concurrency::mpmc_channel<i32>(1);
        rx.clear();
        auto sent = Async::send(pool, tx, 3).block();
        assert(!sent.ok());
        assert(sent.unwrap_err() == Concurrency::Channel_Error::disconnected);
    }

    Trace("Async recv_for timeout") {
        auto [tx, rx] = Concurrency::mpmc_channel<i32>(1);
        auto got = Async::recv_for(pool, rx, 5).block();
        assert(!got.ok());
        assert(got.unwrap_err() == Concurrency::Channel_Error::timeout);
        tx.close();
    }

    Trace("Async send_for timeout") {
        auto [tx, rx] = Concurrency::mpmc_channel<i32>(1);
        assert(tx.send(1).ok());
        auto sent = Async::send_for(pool, tx, 2, 5).block();
        assert(!sent.ok());
        assert(sent.unwrap_err() == Concurrency::Channel_Error::timeout);
        rx.close();
    }

    return 0;
}
