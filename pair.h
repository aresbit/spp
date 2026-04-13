
#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

namespace spp {

template<typename A, typename B>
struct Pair {

    constexpr explicit Pair() noexcept
        requires Default_Constructable<A> && Default_Constructable<B>
    = default;

    constexpr explicit Pair(const A& first, const B& second) noexcept
        requires Copy_Constructable<A> && Copy_Constructable<B>
        : first(A{first}), second(B{second}) {
    }

    constexpr explicit Pair(A&& first, B&& second) noexcept
        requires Move_Constructable<A> && Move_Constructable<B>
        : first(spp::move(first)), second(spp::move(second)) {
    }

    constexpr explicit Pair(const A& first, B&& second) noexcept
        requires Copy_Constructable<A> && Move_Constructable<B>
        : first(A{first}), second(spp::move(second)) {
    }

    constexpr explicit Pair(A&& first, const B& second) noexcept
        requires Move_Constructable<A> && Copy_Constructable<B>
        : first(spp::move(first)), second(B{second}) {
    }

    constexpr ~Pair() noexcept = default;

    constexpr Pair(const Pair& src) noexcept
        requires Copy_Constructable<A> && Copy_Constructable<B>
    = default;
    constexpr Pair& operator=(const Pair& src) noexcept
        requires Copy_Constructable<A> && Copy_Constructable<B>
    = default;

    constexpr Pair(Pair&& src) noexcept = default;
    constexpr Pair& operator=(Pair&& src) noexcept = default;

    [[nodiscard]] constexpr Pair<A, B> clone() const noexcept
        requires(Clone<A> || Copy_Constructable<A>) && (Clone<B> || Copy_Constructable<B>)
    {
        if constexpr(Clone<A> && Clone<B>) {
            return Pair<A, B>{first.clone(), second.clone()};
        } else if constexpr(Clone<A> && Copy_Constructable<B>) {
            return Pair<A, B>{first.clone(), second};
        } else if constexpr(Copy_Constructable<A> && Clone<B>) {
            return Pair<A, B>{first, second.clone()};
        } else {
            static_assert(Copy_Constructable<A> && Copy_Constructable<B>);
            return Pair<A, B>{first, second};
        }
    }

    template<u64 Index>
    [[nodiscard]] constexpr auto& get() noexcept {
        if constexpr(Index == 0) return first;
        if constexpr(Index == 1) return second;
    }
    template<u64 Index>
    [[nodiscard]] constexpr const auto& get() const noexcept {
        if constexpr(Index == 0) return first;
        if constexpr(Index == 1) return second;
    }

    A first;
    B second;
};

template<typename A, typename B>
SPP_TEMPLATE_RECORD(Pair, SPP_PACK(A, B), SPP_FIELD(first), SPP_FIELD(second));

namespace Format {

template<Reflectable L, Reflectable R>
struct Measure<Pair<L, R>> {
    [[nodiscard]] constexpr static u64 measure(const Pair<L, R>& pair) noexcept {
        return 8 + Measure<L>::measure(pair.first) + Measure<R>::measure(pair.second);
    }
};
template<Allocator O, Reflectable L, Reflectable R>
struct Write<O, Pair<L, R>> {
    static u64 write(String<O>& output, u64 idx, const Pair<L, R>& pair) noexcept {
        idx = output.write(idx, "Pair{"_v);
        idx = Write<O, L>::write(output, idx, pair.first);
        idx = output.write(idx, ", "_v);
        idx = Write<O, R>::write(output, idx, pair.second);
        return output.write(idx, '}');
    }
};

} // namespace Format

} // namespace spp

namespace std {

using spp::Pair;

template<typename T>
struct tuple_size;

template<spp::u64 I, typename T>
struct tuple_element;

template<typename L, typename R>
struct tuple_size<Pair<L, R>> : spp::Constant<spp::u64, 2> {};

template<typename L, typename R>
struct tuple_element<0, Pair<L, R>> {
    using type = L;
};
template<typename L, typename R>
struct tuple_element<1, Pair<L, R>> {
    using type = R;
};

} // namespace std
