
#pragma once

#include "base.h"
#include "function.h"

namespace spp {
namespace Log {

using Callback = void(Level, Thread::Id, Time, Location, String_View);
using Token = u64;

[[nodiscard]] Token subscribe(Function<Callback> f) noexcept;
void unsubscribe(Token token) noexcept;

template<Invocable<Level, Thread::Id, Time, Location, String_View> F>
[[nodiscard]] Token subscribe(F&& f) noexcept {
    return subscribe(Function<Callback>{spp::forward<F>(f)});
}

} // namespace Log
} // namespace spp
