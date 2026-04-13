
#include <spp/io/files.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace spp::Files {

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
        close(fd);
        return File_Result<Vec<u8, Alloc>>::err("read_failed"_v);
    }

    close(fd);
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
        close(fd);
        return File_Result<u64>::err("write_failed"_v);
    }

    close(fd);
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
        close(fd);
        return File_Result<u64>::err("pread_failed"_v);
    }
    close(fd);
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
        close(fd);
        return File_Result<u64>::err("pwrite_failed"_v);
    }
    close(fd);
    return File_Result<u64>::ok(static_cast<u64>(ret));
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
