
#include <spp/io/files.h>
#include <spp/io/handle.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace spp::Files {

namespace {

inline void close_fd(int fd) noexcept {
    if(fd < 0) return;
    auto handle = IO::file_handle(static_cast<uptr>(fd));
    auto closed = IO::close_result(handle);
    if(!closed.ok()) {
        warn("Failed to close file handle: %", closed.unwrap_err());
    }
}

} // namespace

[[nodiscard]] File_Result<Vec<u8, Alloc>> read_result(String_View path_) noexcept {

    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_RDONLY);
    }

    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<Vec<u8, Alloc>>::err("open_failed"_v);
    }

    off_t full_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    assert(full_size <= SPP_UINT32_MAX);

    Vec<u8, Alloc> data(static_cast<u64>(full_size));
    data.resize(static_cast<u64>(full_size));

    if(::read(fd, data.data(), full_size) == -1) {
        warn("Failed to read file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<Vec<u8, Alloc>>::err("read_failed"_v);
    }

    close_fd(fd);
    return File_Result<Vec<u8, Alloc>>::ok(spp::move(data));
}

[[nodiscard]] File_Result<u64> write_result(String_View path_, Slice<const u8> data) noexcept {

    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }

    if(fd == -1) {
        warn("Failed to create file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("create_failed"_v);
    }

    if(::write(fd, data.data(), data.length()) == -1) {
        warn("Failed to write file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("write_failed"_v);
    }

    close_fd(fd);
    return File_Result<u64>::ok(data.length());
}

[[nodiscard]] File_Result<u64> pread_result(String_View path_, u64 offset, Slice<u8> out) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_RDONLY);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    ssize_t ret = pread(fd, out.data(), out.length(), static_cast<off_t>(offset));
    if(ret == -1) {
        warn("Failed to pread file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("pread_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(static_cast<u64>(ret));
}

[[nodiscard]] File_Result<u64> pwrite_result(String_View path_, u64 offset,
                                             Slice<const u8> data) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT, 0644);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    ssize_t ret = pwrite(fd, data.data(), data.length(), static_cast<off_t>(offset));
    if(ret == -1) {
        warn("Failed to pwrite file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("pwrite_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(static_cast<u64>(ret));
}

[[nodiscard]] File_Result<u64> preadv_result(String_View path_, u64 offset,
                                             Slice<Read_IO_Slice> outs) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_RDONLY);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    u64 total = 0;
    off_t cur = static_cast<off_t>(offset);
    for(u64 i = 0; i < outs.length(); i++) {
        auto out = outs[i];
        if(out.length == 0) continue;
        ssize_t ret = pread(fd, out.data, out.length, cur);
        if(ret == -1) {
            warn("Failed to preadv file %: %", path_, Log::sys_error());
            close_fd(fd);
            return File_Result<u64>::err("preadv_failed"_v);
        }
        total += static_cast<u64>(ret);
        cur += static_cast<off_t>(ret);
        if(static_cast<u64>(ret) < out.length) break;
    }
    close_fd(fd);
    return File_Result<u64>::ok(spp::move(total));
}

[[nodiscard]] File_Result<u64> pwritev_result(String_View path_, u64 offset,
                                              Slice<const Write_IO_Slice> inputs) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT, 0644);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    u64 total = 0;
    off_t cur = static_cast<off_t>(offset);
    for(u64 i = 0; i < inputs.length(); i++) {
        auto in = inputs[i];
        if(in.length == 0) continue;
        ssize_t ret = pwrite(fd, in.data, in.length, cur);
        if(ret == -1) {
            warn("Failed to pwritev file %: %", path_, Log::sys_error());
            close_fd(fd);
            return File_Result<u64>::err("pwritev_failed"_v);
        }
        total += static_cast<u64>(ret);
        cur += static_cast<off_t>(ret);
        if(static_cast<u64>(ret) < in.length) break;
    }
    close_fd(fd);
    return File_Result<u64>::ok(spp::move(total));
}

[[nodiscard]] File_Result<u64> truncate_result(String_View path_, u64 size) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT, 0644);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }
    if(ftruncate(fd, static_cast<off_t>(size)) == -1) {
        warn("Failed to truncate file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("truncate_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(spp::move(size));
}

[[nodiscard]] File_Result<u64> fsync_result(String_View path_) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }
    if(fsync(fd) == -1) {
        warn("Failed to fsync file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("fsync_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> fdatasync_result(String_View path_) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

#if defined(__APPLE__)
    int ret = fcntl(fd, F_FULLFSYNC);
#else
    int ret = fdatasync(fd);
#endif
    if(ret == -1) {
        warn("Failed to fdatasync file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("fdatasync_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<bool> exists_result(String_View path_) noexcept {
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        struct stat info;
        if(stat(reinterpret_cast<const char*>(path.data()), &info) == 0) {
            return File_Result<bool>::ok(true);
        }
        if(errno == ENOENT) {
            return File_Result<bool>::ok(false);
        }
        warn("Failed to stat file %: %", path_, Log::sys_error());
        return File_Result<bool>::err("stat_failed"_v);
    }
}

[[nodiscard]] File_Result<u64> remove_result(String_View path_) noexcept {
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        if(unlink(reinterpret_cast<const char*>(path.data())) == -1) {
            warn("Failed to remove file %: %", path_, Log::sys_error());
            return File_Result<u64>::err("remove_failed"_v);
        }
        return File_Result<u64>::ok(0);
    }
}

[[nodiscard]] File_Result<u64> rename_result(String_View from_, String_View to_) noexcept {
    Region(R) {
        auto from = from_.terminate<Mregion<R>>();
        auto to = to_.terminate<Mregion<R>>();
        if(::rename(reinterpret_cast<const char*>(from.data()), reinterpret_cast<const char*>(to.data())) ==
           -1) {
            warn("Failed to rename file % -> %: %", from_, to_, Log::sys_error());
            return File_Result<u64>::err("rename_failed"_v);
        }
        return File_Result<u64>::ok(0);
    }
}

[[nodiscard]] File_Result<u64> mkdir_result(String_View path_) noexcept {
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        if(mkdir(reinterpret_cast<const char*>(path.data()), 0755) == -1) {
            if(errno == EEXIST) return File_Result<u64>::ok(0);
            warn("Failed to create directory %: %", path_, Log::sys_error());
            return File_Result<u64>::err("mkdir_failed"_v);
        }
        return File_Result<u64>::ok(0);
    }
}

[[nodiscard]] File_Result<u64> rmdir_result(String_View path_) noexcept {
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        if(rmdir(reinterpret_cast<const char*>(path.data())) == -1) {
            warn("Failed to remove directory %: %", path_, Log::sys_error());
            return File_Result<u64>::err("rmdir_failed"_v);
        }
        return File_Result<u64>::ok(0);
    }
}

[[nodiscard]] File_Result<u64> acquire_lock_result(String_View lock_path_) noexcept {
    int fd = -1;
    Region(R) {
        auto lock_path = lock_path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(lock_path.data()), O_WRONLY | O_CREAT | O_EXCL, 0644);
    }
    if(fd == -1) {
        if(errno == EEXIST) return File_Result<u64>::err("lock_exists"_v);
        warn("Failed to acquire lock %: %", lock_path_, Log::sys_error());
        return File_Result<u64>::err("lock_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> release_lock_result(String_View lock_path_) noexcept {
    Region(R) {
        auto lock_path = lock_path_.terminate<Mregion<R>>();
        if(unlink(reinterpret_cast<const char*>(lock_path.data())) == -1) {
            if(errno == ENOENT) return File_Result<u64>::ok(0);
            warn("Failed to release lock %: %", lock_path_, Log::sys_error());
            return File_Result<u64>::err("unlock_failed"_v);
        }
        return File_Result<u64>::ok(0);
    }
}

[[nodiscard]] File_Result<Mapped_File> mmap_result(String_View path_, u64 size) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_RDWR | O_CREAT, 0644);
    }
    if(fd == -1) {
        warn("Failed to open mmap file %: %", path_, Log::sys_error());
        return File_Result<Mapped_File>::err("open_failed"_v);
    }

    struct stat info;
    if(fstat(fd, &info) == -1) {
        warn("Failed to stat mmap file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<Mapped_File>::err("stat_failed"_v);
    }

    u64 map_len = size == 0 ? static_cast<u64>(info.st_size) : size;
    if(size != 0 && static_cast<u64>(info.st_size) < size) {
        if(ftruncate(fd, static_cast<off_t>(size)) == -1) {
            warn("Failed to extend mmap file %: %", path_, Log::sys_error());
            close_fd(fd);
            return File_Result<Mapped_File>::err("truncate_failed"_v);
        }
    }
    if(map_len == 0) {
        close_fd(fd);
        return File_Result<Mapped_File>::err("map_empty"_v);
    }

    void* ptr = ::mmap(null, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close_fd(fd);
    if(ptr == MAP_FAILED) {
        warn("Failed to mmap file %: %", path_, Log::sys_error());
        return File_Result<Mapped_File>::err("mmap_failed"_v);
    }

    Mapped_File mapped;
    mapped.data = reinterpret_cast<u8*>(ptr);
    mapped.length = map_len;
    return File_Result<Mapped_File>::ok(spp::move(mapped));
}

[[nodiscard]] File_Result<u64> madvise_result(const Mapped_File& mapped, Madvise_Hint hint) noexcept {
    if(mapped.data == null || mapped.length == 0) return File_Result<u64>::err("invalid_map"_v);
    int advice = MADV_NORMAL;
    switch(hint) {
        case Madvise_Hint::normal: advice = MADV_NORMAL; break;
        case Madvise_Hint::sequential: advice = MADV_SEQUENTIAL; break;
        case Madvise_Hint::random: advice = MADV_RANDOM; break;
        case Madvise_Hint::willneed: advice = MADV_WILLNEED; break;
        case Madvise_Hint::dontneed: advice = MADV_DONTNEED; break;
    }
    if(::madvise(mapped.data, mapped.length, advice) == -1) {
        warn("Failed to madvise mapped file: %", Log::sys_error());
        return File_Result<u64>::err("madvise_failed"_v);
    }
    u64 advised = mapped.length;
    return File_Result<u64>::ok(spp::move(advised));
}

[[nodiscard]] File_Result<u64> fallocate_result(String_View path_, u64 size) noexcept {
    int fd = -1;
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();
        fd = open(reinterpret_cast<const char*>(path.data()), O_WRONLY | O_CREAT, 0644);
    }
    if(fd == -1) {
        warn("Failed to open file %: %", path_, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    int ret = 0;
#if defined(__linux__)
    ret = posix_fallocate(fd, 0, static_cast<off_t>(size));
    if(ret != 0) errno = ret;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    ret = ftruncate(fd, static_cast<off_t>(size));
#else
    ret = ftruncate(fd, static_cast<off_t>(size));
#endif
    if(ret != 0) {
        warn("Failed to fallocate file %: %", path_, Log::sys_error());
        close_fd(fd);
        return File_Result<u64>::err("fallocate_failed"_v);
    }
    close_fd(fd);
    return File_Result<u64>::ok(spp::move(size));
}

[[nodiscard]] File_Result<u64> msync_result(const Mapped_File& mapped) noexcept {
    if(mapped.data == null || mapped.length == 0) return File_Result<u64>::err("invalid_map"_v);
    if(::msync(mapped.data, mapped.length, MS_SYNC) == -1) {
        warn("Failed to msync mapped file: %", Log::sys_error());
        return File_Result<u64>::err("msync_failed"_v);
    }
    u64 synced = mapped.length;
    return File_Result<u64>::ok(spp::move(synced));
}

[[nodiscard]] File_Result<u64> munmap_result(Mapped_File& mapped) noexcept {
    if(mapped.data == null || mapped.length == 0) return File_Result<u64>::err("invalid_map"_v);
    if(::munmap(mapped.data, mapped.length) == -1) {
        warn("Failed to munmap file: %", Log::sys_error());
        return File_Result<u64>::err("munmap_failed"_v);
    }
    u64 len = mapped.length;
    mapped.data = null;
    mapped.length = 0;
    mapped.handle0 = 0;
    mapped.handle1 = 0;
    return File_Result<u64>::ok(spp::move(len));
}

[[nodiscard]] File_Result<File_Time> last_write_time_result(String_View path_) noexcept {
    Region(R) {
        auto path = path_.terminate<Mregion<R>>();

        struct stat info;
        if(stat(reinterpret_cast<const char*>(path.data()), &info)) {
            warn("Failed to stat file %: %", path_, Log::sys_error());
            return File_Result<File_Time>::err("stat_failed"_v);
        }
        return File_Result<File_Time>::ok(static_cast<File_Time>(info.st_mtime));
    }
}

[[nodiscard]] bool before(const File_Time& first, const File_Time& second) noexcept {
    return first < second;
}

} // namespace spp::Files
