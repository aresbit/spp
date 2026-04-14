
#pragma once

#include <spp/async/async.h>
#include <spp/io/files.h>
#include <spp/async/pool.h>

namespace spp::Async {

enum class Wait_Error : u8 {
    cancelled,
    timer_create_failed,
    timer_set_failed,
};

[[nodiscard]] inline String_View wait_error_string(Wait_Error err) noexcept {
    switch(err) {
        case Wait_Error::cancelled: return "cancelled"_v;
        case Wait_Error::timer_create_failed: return "timer_create_failed"_v;
        case Wait_Error::timer_set_failed: return "timer_set_failed"_v;
    }
    SPP_UNREACHABLE;
}

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

[[nodiscard]] Task<Result<u64, Wait_Error>> wait_typed(Pool<>& pool, u64 ms) noexcept;
[[nodiscard]] Task<Result<u64, Wait_Error>> wait_typed(Pool<>& pool, u64 ms,
                                                       const Cancel_Token& token) noexcept;

[[nodiscard]] Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms,
                                                         const Cancel_Token& token) noexcept;
[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept;

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path,
                                                          Slice<u8> data) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> pread_result(Pool<>& pool, String_View path,
                                                          u64 offset, Slice<u8> out) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> pwrite_result(Pool<>& pool, String_View path,
                                                           u64 offset,
                                                           Slice<const u8> data) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> preadv_result(Pool<>& pool, String_View path,
                                                           u64 offset,
                                                           Slice<Files::Read_IO_Slice> outs) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> pwritev_result(
    Pool<>& pool, String_View path, u64 offset, Slice<const Files::Write_IO_Slice> inputs) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> fdatasync_result(Pool<>& pool,
                                                              String_View path) noexcept;

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path) noexcept;
[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data) noexcept;

[[nodiscard]] inline Task<Result<u64, Wait_Error>> wait_typed(Pool<>& pool, u64 ms,
                                                              const Cancel_Token& token) noexcept {
    u64 waited = 0;
    while(waited < ms) {
        if(token.cancelled()) {
            co_return Result<u64, Wait_Error>::err(Wait_Error::cancelled);
        }

        u64 step_ms = (ms - waited) > 1 ? 1 : (ms - waited);
        auto step = co_await wait_typed(pool, step_ms);
        if(!step.ok()) {
            co_return Result<u64, Wait_Error>::err(spp::move(step.unwrap_err()));
        }
        waited += step.unwrap();
    }

    co_return Result<u64, Wait_Error>::ok(u64{ms});
}

[[nodiscard]] inline Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms) noexcept {
    auto waited = co_await wait_typed(pool, ms);
    if(!waited.ok()) {
        co_return Result<u64, String_View>::err(wait_error_string(waited.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(u64{waited.unwrap()});
}

[[nodiscard]] inline Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms,
                                                                const Cancel_Token& token) noexcept {
    auto waited = co_await wait_typed(pool, ms, token);
    if(!waited.ok()) {
        co_return Result<u64, String_View>::err(wait_error_string(waited.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(u64{waited.unwrap()});
}

} // namespace spp::Async
