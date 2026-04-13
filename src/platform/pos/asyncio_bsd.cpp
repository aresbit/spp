
#include <spp/async/asyncio.h>
#include <spp/io/files.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// These are not actually async.

namespace spp::Async {

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path_) noexcept {

    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_RDONLY);
    }

    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("open_failed"_v);
    }

    off_t full_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    assert(full_size <= UINT32_MAX);

    Vec<u8, Files::Alloc> data(static_cast<u64>(full_size));
    data.resize(static_cast<u64>(full_size));

    if(::read(fd, data.data(), full_size) == -1) {
        warn("Failed to read file %: %", path_, Log::sys_error());
        close(fd);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("read_failed"_v);
    }

    close(fd);
    co_return Result<Vec<u8, Files::Alloc>, String_View>::ok(spp::move(data));
}

[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path_,
                                                          Slice<u8> data) noexcept {

    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT | O_TRUNC);
    }

    if(fd == -1) {
        warn("Failed to create file %: %", path_, Log::sys_error());
        co_return Result<u64, String_View>::err("create_failed"_v);
    }

    if(::write(fd, data.data(), data.length()) == -1) {
        warn("Failed to write file %: %", path_, Log::sys_error());
        close(fd);
        co_return Result<u64, String_View>::err("write_failed"_v);
    }

    close(fd);
    co_return Result<u64, String_View>::ok(data.length());
}

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path) noexcept {
    auto result = co_await read_result(pool, path);
    if(!result.ok()) co_return Opt<Vec<u8, Files::Alloc>>{};
    co_return Opt{spp::move(result.unwrap())};
}

[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data) noexcept {
    auto result = co_await write_result(pool, path, data);
    co_return result.ok();
}

[[nodiscard]] Task<void> wait(Pool<>&, u64 ms) noexcept {
    Thread::sleep(ms);
    co_return;
}

} // namespace spp::Async
