
#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#ifdef SPP_COMPILER_MSVC
#define SPP_PRETTY_FUNCTION __FUNCTION__
#elif defined SPP_COMPILER_CLANG
#define SPP_PRETTY_FUNCTION __PRETTY_FUNCTION__
#endif

#define SPP_HERE ::spp::Log::Location::make(__FILE__, __LINE__, SPP_PRETTY_FUNCTION)

#define info(fmt, ...)                                                                             \
    (void)(::spp::Log::log(::spp::Log::Level::info, SPP_HERE, fmt##_v, ##__VA_ARGS__), 0)

#define warn(fmt, ...)                                                                             \
    (void)(::spp::Log::log(::spp::Log::Level::warn, SPP_HERE, fmt##_v, ##__VA_ARGS__), 0)

#define die(fmt, ...)                                                                              \
    (void)(::spp::Log::log(::spp::Log::Level::fatal, SPP_HERE, fmt##_v, ##__VA_ARGS__),            \
           SPP_DEBUG_BREAK, ::spp::Libc::exit(1), 0)

#undef assert
#define assert(expr)                                                                               \
    (void)((!!(expr)) ||                                                                           \
           (::spp::Log::log(::spp::Log::Level::fatal, SPP_HERE, ::spp::String_View{"Assert: %"},   \
                            ::spp::String_View{#expr}),                                            \
            SPP_DEBUG_BREAK, ::spp::Libc::exit(1), 0))

#define SPP_UNREACHABLE                                                                            \
    (void)(::spp::Log::log(::spp::Log::Level::fatal, SPP_HERE, ::spp::String_View{"Unreachable"}), \
           SPP_DEBUG_BREAK, ::spp::Libc::exit(1), 0)

#define SPP_DEBUG_BREAK (::spp::Log::debug_break())

#define SPP_INDENT2(COUNTER) __log_scope_##COUNTER
#define SPP_INDENT1(COUNTER) if(::spp::Log::Scope SPP_INDENT2(COUNTER){})

#define Log_Indent SPP_INDENT1(__COUNTER__)

namespace spp {
namespace Log {

constexpr u64 INDENT_SIZE = 4;

enum class Level : u8 {
    info,
    warn,
    fatal,
};

struct Location {
    String_View function;
    String_View file;
    u64 line = 0;

    template<u64 N, u64 M>
    [[nodiscard]] constexpr static Location make(const char (&file)[N], u64 line,
                                                 const char (&function)[M]) noexcept {
        return Location{String_View{function}, String_View{file}.file_suffix(),
                        static_cast<u64>(line)};
    }

    [[nodiscard]] constexpr bool operator==(const Log::Location& other) const noexcept {
        return function == other.function && file == other.file && line == other.line;
    }
};

struct Scope {
    Scope() noexcept;
    ~Scope() noexcept;

    [[nodiscard]] consteval operator bool() noexcept {
        return true;
    }
};

using Time = u64;

[[nodiscard]] Time sys_time() noexcept;
[[nodiscard]] String_View sys_time_string(Time time) noexcept;
[[nodiscard]] String_View sys_error() noexcept;

void debug_break() noexcept;
void output(Level level, const Location& loc, String_View msg) noexcept;

template<typename... Ts>
void log(Level level, const Location& loc, String_View fmt, const Ts&... args) noexcept {
    Region(R) output(level, loc, format<Mregion<R>>(fmt, args...).view());
}

namespace detail {

struct Static_Init {
    Static_Init() noexcept;
    ~Static_Init() noexcept;
};

// Constructed before subsequent globals and destructed after them.
inline Static_Init g_initializer;

}; // namespace detail

} // namespace Log

SPP_NAMED_ENUM(Log::Level, "Level", info, SPP_CASE(info), SPP_CASE(warn), SPP_CASE(fatal));
SPP_NAMED_RECORD(Log::Location, "Location", SPP_FIELD(function), SPP_FIELD(file), SPP_FIELD(line));

namespace Hash {

template<>
struct Hash<Log::Location> {
    [[nodiscard]] constexpr static u64 hash(const Log::Location& l) noexcept {
        return spp::hash(l.file, l.function, l.line);
    }
};

} // namespace Hash

} // namespace spp
