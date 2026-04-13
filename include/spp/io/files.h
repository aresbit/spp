
#pragma once

#include <spp/core/base.h>

namespace spp::Files {

using Alloc = Mallocator<"File IO">;

using File_Time = u64;
template<typename T>
using File_Result = Result<T, String_View>;

[[nodiscard]] File_Result<Vec<u8, Alloc>> read_result(String_View path) noexcept;
[[nodiscard]] File_Result<u64> write_result(String_View path, Slice<const u8> data) noexcept;
[[nodiscard]] File_Result<u64> pread_result(String_View path, u64 offset, Slice<u8> out) noexcept;
[[nodiscard]] File_Result<u64> pwrite_result(String_View path, u64 offset,
                                             Slice<const u8> data) noexcept;
[[nodiscard]] File_Result<u64> truncate_result(String_View path, u64 size) noexcept;
[[nodiscard]] File_Result<u64> fsync_result(String_View path) noexcept;
[[nodiscard]] File_Result<File_Time> last_write_time_result(String_View path) noexcept;

[[nodiscard]] Opt<Vec<u8, Alloc>> read(String_View path) noexcept;
[[nodiscard]] bool write(String_View path, Slice<const u8> data) noexcept;

[[nodiscard]] Opt<File_Time> last_write_time(String_View path) noexcept;

[[nodiscard]] bool before(const File_Time& first, const File_Time& second) noexcept;

struct Write_Watcher {

    explicit Write_Watcher(String_View path) noexcept : path_(spp::move(path)) {
        if(path_.empty()) return;
        auto time = last_write_time_result(path_);
        if(time.ok()) last_write_time_ = time.unwrap();
    }

    [[nodiscard]] String_View path() const noexcept {
        return path_;
    }

    [[nodiscard]] Opt<Vec<u8, Alloc>> read() const noexcept {
        return Files::read(path_);
    }

    [[nodiscard]] bool poll() noexcept {
        if(path_.empty()) return false;
        auto time = last_write_time_result(path_);
        if(!time.ok()) return false;
        bool ret = before(last_write_time_, time.unwrap());
        last_write_time_ = time.unwrap();
        return ret;
    }

private:
    String_View path_;
    File_Time last_write_time_ = 0;
};

inline Opt<Vec<u8, Alloc>> read(String_View path) noexcept {
    auto result = read_result(path);
    if(!result.ok()) return {};
    return Opt{move(result.unwrap())};
}

inline bool write(String_View path, Slice<const u8> data) noexcept {
    return write_result(path, data).ok();
}

inline Opt<File_Time> last_write_time(String_View path) noexcept {
    auto result = last_write_time_result(path);
    if(!result.ok()) return {};
    return Opt{result.unwrap()};
}

} // namespace spp::Files
