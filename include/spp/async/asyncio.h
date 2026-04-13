
#pragma once

#include <spp/async/async.h>
#include <spp/io/files.h>
#include <spp/async/pool.h>

namespace spp::Async {

struct Cancel_Token {
    void cancel() noexcept {
        cancelled_.exchange(1);
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load() != 0;
    }

private:
    Thread::Atomic cancelled_{0};
};

[[nodiscard]] Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms,
                                                         const Cancel_Token& token) noexcept;
[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept;

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path,
                                                          Slice<u8> data) noexcept;

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path) noexcept;
[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data) noexcept;

[[nodiscard]] inline Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms,
                                                                const Cancel_Token& token) noexcept {
    u64 waited = 0;
    while(waited < ms) {
        if(token.cancelled()) {
            co_return Result<u64, String_View>::err("cancelled"_v);
        }

        u64 step_ms = (ms - waited) > 1 ? 1 : (ms - waited);
        auto step = co_await wait_result(pool, step_ms);
        if(!step.ok()) {
            co_return Result<u64, String_View>::err(spp::move(step.unwrap_err()));
        }
        waited += step.unwrap();
    }

    co_return Result<u64, String_View>::ok(u64{ms});
}

} // namespace spp::Async
