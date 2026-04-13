#pragma once

#include <spp/core/variant.h>

namespace spp {

template<typename... Ts, typename... Fs>
[[nodiscard]] auto match(Variant<Ts...>& variant, Fs&&... fs) noexcept {
    return variant.match(Overload{spp::forward<Fs>(fs)...});
}

template<typename... Ts, typename... Fs>
[[nodiscard]] auto match(const Variant<Ts...>& variant, Fs&&... fs) noexcept {
    return variant.match(Overload{spp::forward<Fs>(fs)...});
}

template<typename... Ts, typename... Fs>
[[nodiscard]] auto match(Variant<Ts...>&& variant, Fs&&... fs) noexcept {
    return spp::move(variant).match(Overload{spp::forward<Fs>(fs)...});
}

} // namespace spp
