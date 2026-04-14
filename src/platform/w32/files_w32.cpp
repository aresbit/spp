
#include <spp/io/files.h>

#include <spp/platform/w32_util.h>
#include <windows.h>

namespace spp::Files {

[[nodiscard]] File_Result<File_Time> last_write_time_result(String_View path) noexcept {

    WIN32_FILE_ATTRIBUTE_DATA attrib = {};

    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<File_Time>::err("path_convert_failed"_v);
    }

    if(GetFileAttributesExW(ucs2_path, GetFileExInfoStandard, (LPVOID)&attrib) == 0) {
        warn("Failed to get file attributes %: %", path, Log::sys_error());
        return File_Result<File_Time>::err("attrib_failed"_v);
    }

    return File_Result<File_Time>::ok((static_cast<u64>(attrib.ftLastWriteTime.dwHighDateTime)
                                       << 32) |
                                      static_cast<u64>(attrib.ftLastWriteTime.dwLowDateTime));
}

[[nodiscard]] bool before(const File_Time& first, const File_Time& second) noexcept {
    FILETIME f, s;
    f.dwLowDateTime = static_cast<u32>(first);
    f.dwHighDateTime = static_cast<u32>(first >> 32);
    s.dwLowDateTime = static_cast<u32>(second);
    s.dwHighDateTime = static_cast<u32>(second >> 32);
    return CompareFileTime(&f, &s) == -1;
}

[[nodiscard]] File_Result<Vec<u8, Alloc>> read_result(String_View path) noexcept {

    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<Vec<u8, Alloc>>::err("path_convert_failed"_v);
    }

    HANDLE handle = CreateFileW(ucs2_path, GENERIC_READ, FILE_SHARE_READ, null, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to create file %: %", path, Log::sys_error());
        return File_Result<Vec<u8, Alloc>>::err("open_failed"_v);
    }

    LARGE_INTEGER full_size;
    if(GetFileSizeEx(handle, &full_size) == FALSE) {
        warn("Failed to size file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<Vec<u8, Alloc>>::err("size_failed"_v);
    }

    u64 size = static_cast<u64>(full_size.QuadPart);
    assert(size <= SPP_UINT32_MAX);

    Vec<u8, Alloc> data(size);
    data.resize(size);

    if(ReadFile(handle, data.data(), static_cast<u32>(size), null, null) == FALSE) {
        warn("Failed to read file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<Vec<u8, Alloc>>::err("read_failed"_v);
    }

    CloseHandle(handle);

    return File_Result<Vec<u8, Alloc>>::ok(spp::move(data));
}

[[nodiscard]] File_Result<u64> write_result(String_View path, Slice<const u8> data) noexcept {

    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    HANDLE handle =
        CreateFileW(ucs2_path, GENERIC_WRITE, 0, null, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to create file %: %", path, Log::sys_error());
        return File_Result<u64>::err("create_failed"_v);
    }

    assert(data.length() <= SPP_UINT32_MAX);

    if(WriteFile(handle, data.data(), static_cast<u32>(data.length()), null, null) == FALSE) {
        warn("Failed to write file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("write_failed"_v);
    }

    CloseHandle(handle);
    return File_Result<u64>::ok(data.length());
}

[[nodiscard]] File_Result<u64> pread_result(String_View path, u64 offset, Slice<u8> out) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    HANDLE handle = CreateFileW(ucs2_path, GENERIC_READ, FILE_SHARE_READ, null, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to open file %: %", path, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if(SetFilePointerEx(handle, pos, null, FILE_BEGIN) == FALSE) {
        warn("Failed to seek file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("seek_failed"_v);
    }

    assert(out.length() <= SPP_UINT32_MAX);
    DWORD read_bytes = 0;
    if(ReadFile(handle, out.data(), static_cast<u32>(out.length()), &read_bytes, null) == FALSE) {
        warn("Failed to pread file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("pread_failed"_v);
    }

    CloseHandle(handle);
    return File_Result<u64>::ok(static_cast<u64>(read_bytes));
}

[[nodiscard]] File_Result<u64> pwrite_result(String_View path, u64 offset,
                                             Slice<const u8> data) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    HANDLE handle =
        CreateFileW(ucs2_path, GENERIC_WRITE, 0, null, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to open file %: %", path, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if(SetFilePointerEx(handle, pos, null, FILE_BEGIN) == FALSE) {
        warn("Failed to seek file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("seek_failed"_v);
    }

    assert(data.length() <= SPP_UINT32_MAX);
    DWORD written = 0;
    if(WriteFile(handle, data.data(), static_cast<u32>(data.length()), &written, null) == FALSE) {
        warn("Failed to pwrite file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("pwrite_failed"_v);
    }

    CloseHandle(handle);
    return File_Result<u64>::ok(static_cast<u64>(written));
}

[[nodiscard]] File_Result<u64> preadv_result(String_View path, u64 offset,
                                             Slice<Read_IO_Slice> outs) noexcept {
    u64 total = 0;
    u64 cur = offset;
    for(u64 i = 0; i < outs.length(); i++) {
        auto out = outs[i];
        if(out.length == 0) continue;
        auto ret = pread_result(path, cur, Slice<u8>{out.data, out.length});
        if(!ret.ok()) return ret;
        u64 got = ret.unwrap();
        total += got;
        cur += got;
        if(got < out.length) break;
    }
    return File_Result<u64>::ok(spp::move(total));
}

[[nodiscard]] File_Result<u64> pwritev_result(String_View path, u64 offset,
                                              Slice<const Write_IO_Slice> inputs) noexcept {
    u64 total = 0;
    u64 cur = offset;
    for(u64 i = 0; i < inputs.length(); i++) {
        auto in = inputs[i];
        if(in.length == 0) continue;
        auto ret = pwrite_result(path, cur, Slice<const u8>{in.data, in.length});
        if(!ret.ok()) return ret;
        u64 put = ret.unwrap();
        total += put;
        cur += put;
        if(put < in.length) break;
    }
    return File_Result<u64>::ok(spp::move(total));
}

[[nodiscard]] File_Result<u64> truncate_result(String_View path, u64 size) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    HANDLE handle =
        CreateFileW(ucs2_path, GENERIC_WRITE, 0, null, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to open file %: %", path, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size);
    if(SetFilePointerEx(handle, pos, null, FILE_BEGIN) == FALSE) {
        warn("Failed to seek file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("seek_failed"_v);
    }
    if(SetEndOfFile(handle) == FALSE) {
        warn("Failed to truncate file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("truncate_failed"_v);
    }

    CloseHandle(handle);
    return File_Result<u64>::ok(spp::move(size));
}

[[nodiscard]] File_Result<u64> fsync_result(String_View path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    HANDLE handle = CreateFileW(ucs2_path, GENERIC_WRITE, 0, null, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        warn("Failed to open file %: %", path, Log::sys_error());
        return File_Result<u64>::err("open_failed"_v);
    }

    if(FlushFileBuffers(handle) == FALSE) {
        warn("Failed to fsync file %: %", path, Log::sys_error());
        CloseHandle(handle);
        return File_Result<u64>::err("fsync_failed"_v);
    }

    CloseHandle(handle);
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> fdatasync_result(String_View path) noexcept {
    return fsync_result(path);
}

[[nodiscard]] File_Result<bool> exists_result(String_View path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<bool>::err("path_convert_failed"_v);
    }

    DWORD attr = GetFileAttributesW(ucs2_path);
    if(attr == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        if(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return File_Result<bool>::ok(false);
        }
        warn("Failed to stat file %: %", path, Log::sys_error());
        return File_Result<bool>::err("stat_failed"_v);
    }
    return File_Result<bool>::ok(true);
}

[[nodiscard]] File_Result<u64> remove_result(String_View path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }
    if(DeleteFileW(ucs2_path) == FALSE) {
        warn("Failed to remove file %: %", path, Log::sys_error());
        return File_Result<u64>::err("remove_failed"_v);
    }
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> rename_result(String_View from, String_View to) noexcept {
    auto [from_ucs2, from_len] = utf8_to_ucs2(from);
    if(from_len == 0) {
        warn("Failed to convert file path %!", from);
        return File_Result<u64>::err("path_convert_failed"_v);
    }
    auto [to_ucs2, to_len] = utf8_to_ucs2(to);
    if(to_len == 0) {
        warn("Failed to convert file path %!", to);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    if(MoveFileExW(from_ucs2, to_ucs2, MOVEFILE_REPLACE_EXISTING) == FALSE) {
        warn("Failed to rename file % -> %: %", from, to, Log::sys_error());
        return File_Result<u64>::err("rename_failed"_v);
    }
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> mkdir_result(String_View path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    if(CreateDirectoryW(ucs2_path, null) == FALSE) {
        DWORD err = GetLastError();
        if(err == ERROR_ALREADY_EXISTS) return File_Result<u64>::ok(0);
        warn("Failed to create directory %: %", path, Log::sys_error());
        return File_Result<u64>::err("mkdir_failed"_v);
    }
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> rmdir_result(String_View path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }
    if(RemoveDirectoryW(ucs2_path) == FALSE) {
        warn("Failed to remove directory %: %", path, Log::sys_error());
        return File_Result<u64>::err("rmdir_failed"_v);
    }
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> acquire_lock_result(String_View lock_path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(lock_path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", lock_path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    HANDLE handle =
        CreateFileW(ucs2_path, GENERIC_WRITE, 0, null, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, null);
    if(handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if(err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            return File_Result<u64>::err("lock_exists"_v);
        }
        warn("Failed to acquire lock %: %", lock_path, Log::sys_error());
        return File_Result<u64>::err("lock_failed"_v);
    }
    CloseHandle(handle);
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<u64> release_lock_result(String_View lock_path) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(lock_path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", lock_path);
        return File_Result<u64>::err("path_convert_failed"_v);
    }

    if(DeleteFileW(ucs2_path) == FALSE) {
        DWORD err = GetLastError();
        if(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return File_Result<u64>::ok(0);
        }
        warn("Failed to release lock %: %", lock_path, Log::sys_error());
        return File_Result<u64>::err("unlock_failed"_v);
    }
    return File_Result<u64>::ok(0);
}

[[nodiscard]] File_Result<Mapped_File> mmap_result(String_View path, u64 size) noexcept {
    auto [ucs2_path, ucs2_path_len] = utf8_to_ucs2(path);
    if(ucs2_path_len == 0) {
        warn("Failed to convert file path %!", path);
        return File_Result<Mapped_File>::err("path_convert_failed"_v);
    }

    HANDLE file = CreateFileW(ucs2_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, null,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, null);
    if(file == INVALID_HANDLE_VALUE) {
        warn("Failed to open mmap file %: %", path, Log::sys_error());
        return File_Result<Mapped_File>::err("open_failed"_v);
    }

    LARGE_INTEGER current_size = {};
    if(GetFileSizeEx(file, &current_size) == FALSE) {
        warn("Failed to size mmap file %: %", path, Log::sys_error());
        CloseHandle(file);
        return File_Result<Mapped_File>::err("size_failed"_v);
    }

    u64 map_len = size == 0 ? static_cast<u64>(current_size.QuadPart) : size;
    if(size != 0 && static_cast<u64>(current_size.QuadPart) < size) {
        LARGE_INTEGER new_size;
        new_size.QuadPart = static_cast<LONGLONG>(size);
        if(SetFilePointerEx(file, new_size, null, FILE_BEGIN) == FALSE || SetEndOfFile(file) == FALSE) {
            warn("Failed to extend mmap file %: %", path, Log::sys_error());
            CloseHandle(file);
            return File_Result<Mapped_File>::err("truncate_failed"_v);
        }
    }
    if(map_len == 0) {
        CloseHandle(file);
        return File_Result<Mapped_File>::err("map_empty"_v);
    }

    HANDLE mapping = CreateFileMappingW(file, null, PAGE_READWRITE, static_cast<DWORD>(map_len >> 32),
                                        static_cast<DWORD>(map_len & 0xffffffffull), null);
    CloseHandle(file);
    if(mapping == null) {
        warn("Failed to create file mapping %: %", path, Log::sys_error());
        return File_Result<Mapped_File>::err("mmap_failed"_v);
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                               static_cast<SIZE_T>(map_len));
    if(view == null) {
        warn("Failed to map view file %: %", path, Log::sys_error());
        CloseHandle(mapping);
        return File_Result<Mapped_File>::err("mmap_failed"_v);
    }

    Mapped_File mapped;
    mapped.data = reinterpret_cast<u8*>(view);
    mapped.length = map_len;
    mapped.handle0 = reinterpret_cast<uptr>(mapping);
    return File_Result<Mapped_File>::ok(spp::move(mapped));
}

[[nodiscard]] File_Result<u64> madvise_result(const Mapped_File& mapped, Madvise_Hint) noexcept {
    if(mapped.data == null || mapped.length == 0) return File_Result<u64>::err("invalid_map"_v);
    u64 advised = mapped.length;
    return File_Result<u64>::ok(spp::move(advised));
}

[[nodiscard]] File_Result<u64> fallocate_result(String_View path, u64 size) noexcept {
    return truncate_result(path, size);
}

[[nodiscard]] File_Result<u64> msync_result(const Mapped_File& mapped) noexcept {
    if(mapped.data == null || mapped.length == 0) return File_Result<u64>::err("invalid_map"_v);
    if(FlushViewOfFile(mapped.data, static_cast<SIZE_T>(mapped.length)) == FALSE) {
        warn("Failed to msync mapped file: %", Log::sys_error());
        return File_Result<u64>::err("msync_failed"_v);
    }
    u64 synced = mapped.length;
    return File_Result<u64>::ok(spp::move(synced));
}

[[nodiscard]] File_Result<u64> munmap_result(Mapped_File& mapped) noexcept {
    if(mapped.data == null || mapped.length == 0) return File_Result<u64>::err("invalid_map"_v);
    if(UnmapViewOfFile(mapped.data) == FALSE) {
        warn("Failed to munmap mapped file: %", Log::sys_error());
        return File_Result<u64>::err("munmap_failed"_v);
    }
    if(mapped.handle0 != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(mapped.handle0));
    }
    u64 len = mapped.length;
    mapped.data = null;
    mapped.length = 0;
    mapped.handle0 = 0;
    mapped.handle1 = 0;
    return File_Result<u64>::ok(spp::move(len));
}

} // namespace spp::Files
