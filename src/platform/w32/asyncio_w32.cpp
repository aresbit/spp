
#include <spp/async/asyncio.h>

#include <spp/platform/w32_util.h>
#include <windows.h>

namespace spp::Async {

constexpr u64 SECTOR_SIZE = 4096;

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path) noexcept {

    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("path_convert_failed"_v);
    }

    HANDLE handle =
        CreateFileW(ucs2_path, GENERIC_READ, FILE_SHARE_READ, null, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to create file %: %", path, Log::sys_error());
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("open_failed"_v);
    }

    LARGE_INTEGER full_size;
    if(GetFileSizeEx(handle, &full_size) == FALSE) {
        warn("Failed to size file %: %", path, Log::sys_error());
        CloseHandle(handle);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("size_failed"_v);
    }

    u64 size = static_cast<u64>(full_size.QuadPart);
    u64 aligned_size = Math::align_pow2(size, SECTOR_SIZE);

    assert(aligned_size <= SPP_UINT32_MAX);

    Vec<u8, Files::Alloc> data(aligned_size);
    data.resize(aligned_size);

    HANDLE event = CreateEventEx(null, null, 0, EVENT_ALL_ACCESS);
    if(!event) {
        warn("Failed to create event: %", Log::sys_error());
        CloseHandle(handle);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("event_failed"_v);
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;

    BOOL ret = ReadFile(handle, data.data(), static_cast<u32>(aligned_size), null, &overlapped);
    if(ret == TRUE || GetLastError() != ERROR_IO_PENDING) {
        warn("Failed to initiate async read of file %: %", path, Log::sys_error());
        CloseHandle(event);
        CloseHandle(handle);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("read_failed"_v);
    }

    co_await pool.event(Event::of_sys(event));

    CloseHandle(handle);
    data.resize(size);
    co_return Result<Vec<u8, Files::Alloc>, String_View>::ok(spp::move(data));
}

[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path,
                                                          Slice<u8> data) noexcept {

    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        co_return Result<u64, String_View>::err("path_convert_failed"_v);
    }

    HANDLE handle =
        CreateFileW(ucs2_path, GENERIC_WRITE, 0, null, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to create file %: %", path, Log::sys_error());
        co_return Result<u64, String_View>::err("create_failed"_v);
    }

    HANDLE event = CreateEventEx(null, null, 0, EVENT_ALL_ACCESS);
    if(!event) {
        warn("Failed to create event: %", Log::sys_error());
        CloseHandle(handle);
        co_return Result<u64, String_View>::err("event_failed"_v);
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;

    u64 size = data.length();
    u64 aligned_size = Math::align_pow2(size, SECTOR_SIZE);
    assert(aligned_size <= SPP_UINT32_MAX);

    Vec<u8, Alloc> to_write(aligned_size);
    to_write.resize(aligned_size);

    Libc::memcpy(to_write.data(), data.data(), size);

    BOOL ret =
        WriteFile(handle, to_write.data(), static_cast<u32>(aligned_size), null, &overlapped);
    if(ret == TRUE || GetLastError() != ERROR_IO_PENDING) {
        warn("Failed to initiate async write of file %: %", path, Log::sys_error());
        CloseHandle(event);
        CloseHandle(handle);
        co_return Result<u64, String_View>::err("write_failed"_v);
    }

    co_await pool.event(Event::of_sys(event));

    if(SetFilePointer(handle, static_cast<u32>(size), null, FILE_BEGIN) ==
       INVALID_SET_FILE_POINTER) {
        warn("Failed to set file pointer for file %: %", path, Log::sys_error());
        CloseHandle(handle);
        co_return Result<u64, String_View>::err("seek_failed"_v);
    }

    if(SetEndOfFile(handle) == FALSE) {
        warn("Failed to set end of file for file %: %", path, Log::sys_error());
        CloseHandle(handle);
        co_return Result<u64, String_View>::err("truncate_failed"_v);
    }

    CloseHandle(handle);
    co_return Result<u64, String_View>::ok(size);
}

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path) noexcept {
    auto result = co_await read_result(pool, path);
    if(!result.ok()) co_return Opt<Vec<u8, Files::Alloc>>{};
    co_return Opt{move(result.unwrap())};
}

[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data) noexcept {
    auto result = co_await write_result(pool, path, data);
    co_return result.ok();
}

[[nodiscard]] Task<Result<u64, String_View>> wait_result(Pool<>& pool, u64 ms) noexcept {

    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if(timer == INVALID_HANDLE_VALUE) {
        warn("Failed to create waitable timer: %", Log::sys_error());
        co_return Result<u64, String_View>::err("timer_create_failed"_v);
    }

    LARGE_INTEGER liDueTime;
    liDueTime.QuadPart = -static_cast<LONGLONG>(ms * 10000);

    if(SetWaitableTimer(timer, &liDueTime, 0, NULL, NULL, 0) == FALSE) {
        warn("Failed to set waitable timer: %", Log::sys_error());
        CloseHandle(timer);
        co_return Result<u64, String_View>::err("timer_set_failed"_v);
    }

    co_await pool.event(Event::of_sys(timer));
    co_return Result<u64, String_View>::ok(u64{ms});
}

[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept {
    static_cast<void>(co_await wait_result(pool, ms));
}

} // namespace spp::Async
