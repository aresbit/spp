
#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

// clang-format off

#define SPP_DBL_EPSILON      2.2204460492503131e-016 // smallest such that 1.0+DBL_EPSILON != 1.0
#define SPP_DBL_MAX          1.7976931348623158e+308 // max value
#define SPP_DBL_MIN          2.2250738585072014e-308 // min positive norm
#define SPP_DBL_TRUE_MIN     4.9406564584124654e-324 // min positive denorm
#define SPP_FLT_EPSILON      1.192092896e-07F        // smallest such that 1.0+FLT_EPSILON != 1.0
#define SPP_FLT_MAX          3.402823466e+38F        // max value
#define SPP_FLT_MIN          1.175494351e-38F        // min positive norm
#define SPP_FLT_TRUE_MIN     1.401298464e-45F        // min positive denorm
#define SPP_INT8_MIN         (-127 - 1)
#define SPP_INT16_MIN        (-32767 - 1)
#define SPP_INT32_MIN        (-2147483647 - 1)
#define SPP_INT64_MIN        (-9223372036854775807 - 1)
#define SPP_INT8_MAX         127
#define SPP_INT16_MAX        32767
#define SPP_INT32_MAX        2147483647
#define SPP_INT64_MAX        9223372036854775807
#define SPP_UINT8_MAX        0xff
#define SPP_UINT16_MAX       0xffff
#define SPP_UINT32_MAX       0xffffffff
#define SPP_UINT64_MAX       0xffffffffffffffff

// clang-format on

namespace spp {

template<typename T>
struct Limits;

template<>
struct Limits<u8> {
    [[nodiscard]] consteval static u8 max() noexcept {
        return SPP_UINT8_MAX;
    }
    [[nodiscard]] consteval static u8 min() noexcept {
        return 0;
    }
};

template<>
struct Limits<u16> {
    [[nodiscard]] consteval static u16 max() noexcept {
        return SPP_UINT16_MAX;
    }
    [[nodiscard]] consteval static u16 min() noexcept {
        return 0;
    }
};

template<>
struct Limits<u32> {
    [[nodiscard]] consteval static u32 max() noexcept {
        return SPP_UINT32_MAX;
    }
    [[nodiscard]] consteval static u32 min() noexcept {
        return 0;
    }
};

template<>
struct Limits<u64> {
    [[nodiscard]] consteval static u64 max() noexcept {
        return SPP_UINT64_MAX;
    }
    [[nodiscard]] consteval static u64 min() noexcept {
        return 0;
    }
};

template<>
struct Limits<i8> {
    [[nodiscard]] consteval static i8 max() noexcept {
        return SPP_INT8_MAX;
    }
    [[nodiscard]] consteval static i8 min() noexcept {
        return SPP_INT8_MIN;
    }
};

template<>
struct Limits<i16> {
    [[nodiscard]] consteval static i16 max() noexcept {
        return SPP_INT16_MAX;
    }
    [[nodiscard]] consteval static i16 min() noexcept {
        return SPP_INT16_MIN;
    }
};

template<>
struct Limits<i32> {
    [[nodiscard]] consteval static i32 max() noexcept {
        return SPP_INT32_MAX;
    }
    [[nodiscard]] consteval static i32 min() noexcept {
        return SPP_INT32_MIN;
    }
};

template<>
struct Limits<i64> {
    [[nodiscard]] consteval static i64 max() noexcept {
        return SPP_INT64_MAX;
    }
    [[nodiscard]] consteval static i64 min() noexcept {
        return SPP_INT64_MIN;
    }
};

template<>
struct Limits<f32> {
    [[nodiscard]] consteval static f32 max() noexcept {
        return SPP_FLT_MAX;
    }
    [[nodiscard]] consteval static f32 min() noexcept {
        return -SPP_FLT_MAX;
    }
    [[nodiscard]] consteval static f32 smallest_norm() noexcept {
        return SPP_FLT_MIN;
    }
    [[nodiscard]] consteval static f32 smallest_denorm() noexcept {
        return SPP_FLT_TRUE_MIN;
    }
    [[nodiscard]] consteval static f32 epsilon() noexcept {
        return SPP_FLT_EPSILON;
    }
};

template<>
struct Limits<f64> {
    [[nodiscard]] consteval static f64 max() noexcept {
        return SPP_DBL_MAX;
    }
    [[nodiscard]] consteval static f64 min() noexcept {
        return -SPP_DBL_MAX;
    }
    [[nodiscard]] consteval static f64 smallest_norm() noexcept {
        return SPP_DBL_MIN;
    }
    [[nodiscard]] consteval static f64 smallest_denorm() noexcept {
        return SPP_DBL_TRUE_MIN;
    }
    [[nodiscard]] consteval static f64 epsilon() noexcept {
        return SPP_DBL_EPSILON;
    }
};

} // namespace spp