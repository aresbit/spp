
#include <spp/async/asyncio.h>
#include <spp/io/files.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

// The file IO operations are not actually async.

namespace spp::Async {

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path_) noexcept {
    (void)pool;

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
    if(full_size < 0) {
        warn("Failed to seek file %: %", path_, Log::sys_error());
        close(fd);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("size_failed"_v);
    }
    if(lseek(fd, 0, SEEK_SET) < 0) {
        warn("Failed to rewind file %: %", path_, Log::sys_error());
        close(fd);
        co_return Result<Vec<u8, Files::Alloc>, String_View>::err("seek_failed"_v);
    }

    assert(full_size <= UINT32_MAX);

    Vec<u8, Files::Alloc> data(static_cast<u64>(full_size));
    data.resize(static_cast<u64>(full_size));

    u64 read_total = 0;
    while(read_total < static_cast<u64>(full_size)) {
        ssize_t read_now =
            ::read(fd, data.data() + read_total, static_cast<size_t>(full_size) - read_total);
        if(read_now < 0) {
            if(errno == EINTR) continue;
            warn("Failed to read file %: %", path_, Log::sys_error());
            close(fd);
            co_return Result<Vec<u8, Files::Alloc>, String_View>::err("read_failed"_v);
        }
        if(read_now == 0) {
            warn("Unexpected EOF while reading file %", path_);
            close(fd);
            co_return Result<Vec<u8, Files::Alloc>, String_View>::err("read_failed"_v);
        }
        read_total += static_cast<u64>(read_now);
    }

    close(fd);
    co_return Result<Vec<u8, Files::Alloc>, String_View>::ok(spp::move(data));
}

[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path_,
                                                          Slice<u8> data) noexcept {
    (void)pool;

    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }

    if(fd == -1) {
        warn("Failed to create file %: %", path_, Log::sys_error());
        co_return Result<u64, String_View>::err("create_failed"_v);
    }

    u64 written_total = 0;
    while(written_total < data.length()) {
        ssize_t wrote_now =
            ::write(fd, data.data() + written_total, data.length() - written_total);
        if(wrote_now < 0) {
            if(errno == EINTR) continue;
            warn("Failed to write file %: %", path_, Log::sys_error());
            close(fd);
            co_return Result<u64, String_View>::err("write_failed"_v);
        }
        written_total += static_cast<u64>(wrote_now);
    }

    close(fd);
    co_return Result<u64, String_View>::ok(u64{written_total});
}

[[nodiscard]] Task<Result<u64, String_View>> pread_result(Pool<>& pool, String_View path,
                                                          u64 offset, Slice<u8> out) noexcept {
    (void)pool;
    auto ret = Files::pread_result(path, offset, out);
    if(!ret.ok()) {
        co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
}

[[nodiscard]] Task<Result<u64, String_View>> pwrite_result(Pool<>& pool, String_View path,
                                                           u64 offset,
                                                           Slice<const u8> data) noexcept {
    (void)pool;
    auto ret = Files::pwrite_result(path, offset, data);
    if(!ret.ok()) {
        co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
}

[[nodiscard]] Task<Result<u64, String_View>> preadv_result(Pool<>& pool, String_View path,
                                                           u64 offset,
                                                           Slice<Files::Read_IO_Slice> outs) noexcept {
    (void)pool;
    auto ret = Files::preadv_result(path, offset, outs);
    if(!ret.ok()) {
        co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
}

[[nodiscard]] Task<Result<u64, String_View>> pwritev_result(
    Pool<>& pool, String_View path, u64 offset, Slice<const Files::Write_IO_Slice> inputs) noexcept {
    (void)pool;
    auto ret = Files::pwritev_result(path, offset, inputs);
    if(!ret.ok()) {
        co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
}

[[nodiscard]] Task<Result<u64, String_View>> fdatasync_result(Pool<>& pool,
                                                              String_View path) noexcept {
    (void)pool;
    auto ret = Files::fdatasync_result(path);
    if(!ret.ok()) {
        co_return Result<u64, String_View>::err(spp::move(ret.unwrap_err()));
    }
    co_return Result<u64, String_View>::ok(spp::move(ret.unwrap()));
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

[[nodiscard]] Task<Result<u64, Wait_Error>> wait_typed(Pool<>& pool, u64 ms) noexcept {

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if(fd == -1) {
        warn("Failed to create timerfd: %", Log::sys_error());
        co_return Result<u64, Wait_Error>::err(Wait_Error::timer_create_failed);
    }

    itimerspec spec = {};
    spec.it_value.tv_sec = ms / 1000;
    spec.it_value.tv_nsec = (ms % 1000) * 1000000;

    if(timerfd_settime(fd, 0, &spec, null) == -1) {
        warn("Failed to set timerfd: %", Log::sys_error());
        close(fd);
        co_return Result<u64, Wait_Error>::err(Wait_Error::timer_set_failed);
    }

    co_await pool.event(Async::Event::of_sys(fd, EPOLLIN));
    co_return Result<u64, Wait_Error>::ok(u64{ms});
}

[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept {
    static_cast<void>(co_await wait_typed(pool, ms));
}

} // namespace spp::Async
