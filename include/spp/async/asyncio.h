
#pragma once

#include <spp/async/async.h>
#include <spp/io/files.h>
#include <spp/async/pool.h>

namespace spp::Async {

[[nodiscard]] Task<void> wait(Pool<>& pool, u64 ms) noexcept;

[[nodiscard]] Task<Opt<Vec<u8, Files::Alloc>>> read(Pool<>& pool, String_View path) noexcept;
[[nodiscard]] Task<bool> write(Pool<>& pool, String_View path, Slice<u8> data) noexcept;

} // namespace spp::Async
