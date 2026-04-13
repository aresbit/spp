
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

} // namespace spp::Files
