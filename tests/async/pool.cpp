
#include "test.h"

#include <spp/async/asyncio.h>
#include <spp/async/pool.h>

auto lots_of_jobs(Async::Pool<>& pool, u64 depth) -> Async::Task<u64> {
    if(depth == 0) {
        co_return 1;
    }
    co_await pool.suspend();
    auto job0 = lots_of_jobs(pool, depth - 1);
    auto job1 = lots_of_jobs(pool, depth - 1);
    co_return co_await job0 + co_await job1;
};

i32 main() {
    Test test{"pool"_v};
    {
        Async::Pool pool;

        for(u64 i = 0; i < 10; i++) {
            assert(lots_of_jobs(pool, 8).block() == 256);
        }
    }
    {
        Async::Pool pool;

        {
            auto job = [&pool]() -> Async::Task<i32> {
                co_await pool.suspend();
                info("Hello from coroutine 1 on thread pool");
                co_return 1;
            }();

            info("Job returned %", job.block());
        }
        {
            auto job = [&pool]() -> Async::Task<i32> {
                co_await pool.suspend();
                co_return 1;
            };
            job();
            job();
            job();
        }
    }
    {
        Async::Pool pool;
        {
            auto job = [&pool_ = pool]() -> Async::Task<i32> {
                // pool will be gone after the first suspend, so we need to copy it
                Async::Pool<>& pool = pool_;
                co_await pool.suspend();
                info("Hello from coroutine 4.1 on thread pool");
                co_await pool.suspend();
                info("Hello from coroutine 4.2 on thread pool");
                co_await pool.suspend();
                info("Hello from coroutine 4.3 on thread pool");
                co_return 1;
            };
            static_cast<void>(job().block());
        }
        {
            auto job = [&pool_ = pool]() -> Async::Task<i32> {
                // pool will be gone after the first suspend, so we need to copy it
                Async::Pool<>& pool = pool_;
                co_await pool.suspend();
                co_await pool.suspend();
                co_await pool.suspend();
                co_return 1;
            };
            job();
            job();
            job();
            // These are OK to drop because the promises are refcounted and
            // no job can deadlock itself
        }
    }
    {
        Async::Pool pool;

        {
            auto job = [&pool_ = pool](i32 ms) -> Async::Task<i32> {
                auto& pool = pool_;
                co_await pool.suspend();
                Thread::sleep(ms);
                co_return 1;
            };
            auto job2 = [&pool_ = pool, &job_ = job]() -> Async::Task<i32> {
                auto& pool = pool_;
                auto& job = job_;
                info("5.1 begin");
                co_await pool.suspend();
                info("5.1: co_await 1ms job");
                i32 i = co_await job(1);
                info("5.1: launch 0s job");
                auto wait = job(0);
                info("5.1: co_await 100ms job");
                i32 j = co_await job(100);
                info("5.1: co_await 0s job");
                i32 k = co_await wait;
                info("5.1 done: % % %", i, j, k);
                co_return i + j + k;
            };

            assert(job2().block() == 3);
            // cannot start and drop another job2 because pending continuations
            // are leaked
        }
        {
            Function<Async::Task<void>(u64)> lots_of_jobs =
                [&pool_ = pool, &lots_of_jobs_ = lots_of_jobs](u64 depth) -> Async::Task<void> {
                auto& pool = pool_;
                auto& lots_of_jobs = lots_of_jobs_;
                if(depth == 0) {
                    co_return;
                }
                co_await pool.suspend();
                auto job0 = lots_of_jobs(depth - 1);
                auto job1 = lots_of_jobs(depth - 1);
                co_await job0;
                co_await job1;
            };

            lots_of_jobs(10).block();
        }
    }
    {
        Async::Pool pool;
        {
            auto job = [&pool_ = pool]() -> Async::Task<void> {
                auto& pool = pool_;
                info("coWaiting 100ms.");
                co_await Async::wait(pool, 100);
                info("coWaited 100ms.");
                co_return;
            };

            job().block();
            info("Waited 100ms.");
        }
        {
            auto job = [&pool_ = pool]() -> Async::Task<void> {
                auto& pool = pool_;
                auto waited = co_await Async::wait_result(pool, 10);
                assert(waited.ok());
                assert(waited.unwrap() == 10);
                co_return;
            };

            job().block();
        }
        {
            auto waited = Async::wait_typed(pool, 10).block();
            assert(waited.ok());
            assert(waited.unwrap() == 10);
        }
        {
            Async::Cancel_Token token;
            token.cancel();
            auto job = [&pool_ = pool, &token_ = token]() -> Async::Task<void> {
                auto& pool = pool_;
                auto& token = token_;
                auto waited = co_await Async::wait_result(pool, 10, token);
                assert(!waited.ok());
                assert(waited.unwrap_err() == "cancelled"_v);
                co_return;
            };
            job().block();
        }
        {
            Async::Cancel_Token token;
            token.cancel();
            auto waited = Async::wait_typed(pool, 10, token).block();
            assert(!waited.ok());
            assert(waited.unwrap_err() == Async::Wait_Error::cancelled);
        }
        {
            Async::Cancel_Token token;
            auto canceller = Thread::spawn([&token]() {
                Thread::sleep(5);
                token.cancel();
            });

            auto waited = Async::wait_result(pool, 100, token).block();
            assert(!waited.ok());
            assert(waited.unwrap_err() == "cancelled"_v);
            canceller->block();
        }
        {
            auto stats = pool.stats();
            assert(stats.enqueued > 0);
            assert(stats.stolen <= stats.enqueued);
        }
        {
            constexpr u64 lanes = 24;
            constexpr u64 iterations = 200;

            Vec<Async::Task<u64>> jobs;
            jobs.reserve(lanes);

            for(u64 lane = 0; lane < lanes; lane++) {
                jobs.push([&pool_ = pool]() -> Async::Task<u64> {
                    auto& pool = pool_;
                    u64 progressed = 0;
                    for(u64 i = 0; i < iterations; i++) {
                        co_await pool.suspend();
                        progressed++;
                    }
                    co_return progressed;
                }());
            }

            u64 total_progress = 0;
            for(auto& job : jobs) {
                auto progressed = job.block();
                assert(progressed == iterations);
                total_progress += progressed;
            }
            assert(total_progress == lanes * iterations);
        }
        {
            String_View kPath = "async_pool_io_roundtrip.tmp"_v;
            auto existed = Files::exists_result(kPath);
            assert(existed.ok());
            if(existed.unwrap()) {
                auto removed_existing = Files::remove_result(kPath);
                assert(removed_existing.ok());
            }

            Vec<u8, Files::Alloc> payload;
            for(u8 c : "bsd_read_write_result"_v) payload.push(c);

            auto wrote = Async::write_result(pool, kPath, payload.slice()).block();
            assert(wrote.ok());
            assert(wrote.unwrap() == payload.length());

            auto got = Async::read_result(pool, kPath).block();
            assert(got.ok());
            auto file_data = spp::move(got.unwrap());
            assert(file_data.length() == payload.length());
            for(u64 i = 0; i < payload.length(); i++) {
                assert(file_data[i] == payload[i]);
            }

            auto removed = Files::remove_result(kPath);
            assert(removed.ok());
        }
        {
            String_View kPath = "async_pool_io_positional.tmp"_v;
            auto existed = Files::exists_result(kPath);
            assert(existed.ok());
            if(existed.unwrap()) {
                auto removed_existing = Files::remove_result(kPath);
                assert(removed_existing.ok());
            }

            Array<u8, 32> init{};
            auto wrote_seed = Async::write_result(pool, kPath, init.slice()).block();
            assert(wrote_seed.ok());
            assert(wrote_seed.unwrap() == init.length());

            Array<u8, 2> a{u8{'A'}, u8{'B'}};
            Array<u8, 2> b{u8{'C'}, u8{'D'}};
            Array<u8, 2> c{u8{'E'}, u8{'F'}};

            auto wrote_one = Async::pwrite_result(pool, kPath, 2, a.slice()).block();
            assert(wrote_one.ok());
            assert(wrote_one.unwrap() == a.length());

            Array<Files::Write_IO_Slice, 2> ws{
                Files::Write_IO_Slice{b.data(), b.length()},
                Files::Write_IO_Slice{c.data(), c.length()},
            };
            auto wrote_vec = Async::pwritev_result(pool, kPath, 4, ws.slice()).block();
            assert(wrote_vec.ok());
            assert(wrote_vec.unwrap() == 4);

            auto synced = Async::fdatasync_result(pool, kPath).block();
            assert(synced.ok());

            Array<u8, 2> out_one{};
            auto read_one = Async::pread_result(pool, kPath, 2, out_one.slice()).block();
            assert(read_one.ok());
            assert(read_one.unwrap() == out_one.length());
            assert(out_one[0] == 'A' && out_one[1] == 'B');

            Array<u8, 2> rb{};
            Array<u8, 2> rc{};
            Array<Files::Read_IO_Slice, 2> rs{
                Files::Read_IO_Slice{rb.data(), rb.length()},
                Files::Read_IO_Slice{rc.data(), rc.length()},
            };
            auto read_vec = Async::preadv_result(pool, kPath, 4, rs.slice()).block();
            assert(read_vec.ok());
            assert(read_vec.unwrap() == 4);
            assert(rb[0] == 'C' && rb[1] == 'D');
            assert(rc[0] == 'E' && rc[1] == 'F');

            auto removed = Files::remove_result(kPath);
            assert(removed.ok());
        }
#ifdef SPP_OS_LINUX
        {
            String_View kPath = "async_pool_io_direct_mode.tmp"_v;
            Array<u8, 8> payload{u8{'D'}, u8{'I'}, u8{'R'}, u8{'E'}, u8{'C'}, u8{'T'}, u8{'!'}, u8{'\n'}};
            auto bad_write =
                Async::pwrite_result(pool, kPath, 1, payload.slice(), Async::File_IO_Mode::direct)
                    .block();
            assert(!bad_write.ok());
            assert(bad_write.unwrap_err() == "direct_alignment"_v);
        }
        {
            String_View kPath = "async_pool_io_multiflight.tmp"_v;
            auto existed = Files::exists_result(kPath);
            assert(existed.ok());
            if(existed.unwrap()) {
                auto removed_existing = Files::remove_result(kPath);
                assert(removed_existing.ok());
            }

            Array<u8, 48> init{};
            auto seed = Async::write_result(pool, kPath, init.slice()).block();
            assert(seed.ok());

            constexpr u64 lanes = 8;
            Vec<Async::Task<Result<u64, String_View>>> jobs;
            jobs.reserve(lanes);
            for(u64 i = 0; i < lanes; i++) {
                jobs.push([&pool_ = pool, kPath, i]() -> Async::Task<Result<u64, String_View>> {
                    Array<u8, 4> chunk{
                        static_cast<u8>('A' + i),
                        static_cast<u8>('a' + i),
                        static_cast<u8>('0' + (i % 10)),
                        static_cast<u8>('#'),
                    };
                    co_return co_await Async::pwrite_result(pool_, kPath, i * 4, chunk.slice());
                }());
            }

            for(auto& job : jobs) {
                auto w = job.block();
                assert(w.ok() && w.unwrap() == 4);
            }

            auto all = Async::read_result(pool, kPath).block();
            assert(all.ok());
            auto got = spp::move(all.unwrap());
            for(u64 i = 0; i < lanes; i++) {
                assert(got[i * 4] == static_cast<u8>('A' + i));
                assert(got[i * 4 + 1] == static_cast<u8>('a' + i));
                assert(got[i * 4 + 2] == static_cast<u8>('0' + (i % 10)));
                assert(got[i * 4 + 3] == static_cast<u8>('#'));
            }

            auto removed = Files::remove_result(kPath);
            assert(removed.ok());
        }
#endif
        {
            Vec<Async::Event> events;
            events.push(Async::Event{});
            events.push(Async::Event{});

            auto timeout = Async::Event::wait_any_for(events.slice(), 1);
            assert(!timeout.ok());

            events[1].signal();
            auto signaled = Async::Event::wait_any_for(events.slice(), 100);
            assert(signaled.ok());
            assert(*signaled == 1);
            events[1].reset();
        }
    }
    return 0;
}
