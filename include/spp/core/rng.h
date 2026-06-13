
#pragma once

#include <spp/core/base.h>

namespace spp::RNG {

struct Stream {

    Stream() noexcept {
        seed();
    }
    constexpr Stream(u64 seed) noexcept : state(seed) {
    }

    [[nodiscard]] constexpr u64 operator()() noexcept {
        state = hash(state);
        return state;
    }

    void seed() noexcept {
        state = hash(hash(Thread::this_id()), hash(Log::sys_time()));
    }
    constexpr void seed(u64 seed) noexcept {
        state = seed;
    }

    template<Float F>
    [[nodiscard]] constexpr F unit() noexcept {
        if constexpr(Same<F, f32>) {
            // 24 random bits scaled by 2^-24 (not 10^-24): produces [0, 1).
            u64 r = operator()() >> 40;
            return static_cast<f32>(r) * 0x1p-24f;
        } else {
            static_assert(Same<F, f64>);
            // 53 random bits scaled by 2^-53 (not 10^-53): produces [0, 1).
            u64 r = operator()() >> 11;
            return static_cast<f64>(r) * 0x1p-53;
        }
    }

    template<Float F>
    [[nodiscard]] constexpr bool coin_flip(F p) noexcept {
        return unit<F>() < p;
    }

    template<Allocator A, Move_Constructable T>
    constexpr void shuffle(Vec<T, A>& vec) noexcept {
        for(u64 i = 0; i < vec.length() - 1; i++) {
            u64 j = range(i, vec.length());
            swap(vec[i], vec[j]);
        }
    }

    template<Int I>
    [[nodiscard]] constexpr I integer() noexcept {
        return static_cast<I>(operator()());
    }

    template<Int I>
    [[nodiscard]] constexpr I range(I min, I max) noexcept {
        I r = max - min;
        return min + static_cast<I>(operator()()) % r;
    }

private:
    u64 state = 0;
};

} // namespace spp::RNG
