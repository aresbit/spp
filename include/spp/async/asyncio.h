
#pragma once

#include <spp/async/async.h>
#include <spp/io/files.h>
#include <spp/async/pool.h>

namespace spp::Async {

[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept;

[[nodiscard]] Task<Result<Vec<u8, Files::Alloc>, String_View>> read_result(Pool<>& pool,
                                                                            String_View path) noexcept;
[[nodiscard]] Task<Result<u64, String_View>> write_result(Pool<>& pool, String_View path,
                                                          Slice<u8> data) noexcept;

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path) noexcept;
[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data) noexcept;

} // namespace spp::Async
