
#ifndef SPP_BASE
#error "Include base.h instead."
#endif

namespace spp {

namespace Format {

[[nodiscard]] inline Result<Pair<i64, String_View>, String_View> parse_i64_result(
    String_View input) noexcept;
[[nodiscard]] inline Result<Pair<f32, String_View>, String_View> parse_f32_result(
    String_View input) noexcept;
[[nodiscard]] inline Result<Pair<String_View, String_View>, String_View> parse_string_result(
    String_View s) noexcept;
template<Enum E>
[[nodiscard]] inline Result<Pair<E, String_View>, String_View> parse_enum_result(
    String_View s) noexcept;

template<Enum E>
[[nodiscard]] constexpr Literal enum_name(E value) noexcept {
    Literal ret{"Invalid"};
    iterate_enum<E>([&](const Literal& check, const E& check_value) {
        if(value == check_value) {
            ret = check;
        }
    });
    return ret;
}

[[nodiscard]] inline Opt<Pair<i64, String_View>> parse_i64(String_View input) noexcept {
    auto result = parse_i64_result(input);
    if(!result.ok()) return {};
    return Opt{move(result.unwrap())};
}

[[nodiscard]] inline Result<Pair<i64, String_View>, String_View> parse_i64_result(
    String_View input) noexcept {
    Region(R) {
        auto term = input.terminate<Mregion<R>>();
        const char* start = reinterpret_cast<const char*>(term.data());
        char* end = null;
        i64 ret = Libc::strtoll(start, &end, 10);
        if(start == end) {
            return Result<Pair<i64, String_View>, String_View>::err("parse_i64_failed"_v);
        }
        String_View rest = input.sub(end - start, input.length());
        return Result<Pair<i64, String_View>, String_View>::ok(Pair{ret, spp::move(rest)});
    }
}

[[nodiscard]] inline Opt<Pair<f32, String_View>> parse_f32(String_View input) noexcept {
    auto result = parse_f32_result(input);
    if(!result.ok()) return {};
    return Opt{move(result.unwrap())};
}

[[nodiscard]] inline Result<Pair<f32, String_View>, String_View> parse_f32_result(
    String_View input) noexcept {
    Region(R) {
        auto term = input.terminate<Mregion<R>>();
        const char* start = reinterpret_cast<const char*>(term.data());
        char* end = null;
        f32 ret = Libc::strtof(start, &end);
        if(start == end) {
            return Result<Pair<f32, String_View>, String_View>::err("parse_f32_failed"_v);
        }
        String_View rest = input.sub(end - start, input.length());
        return Result<Pair<f32, String_View>, String_View>::ok(Pair{ret, spp::move(rest)});
    }
}

[[nodiscard]] inline String_View skip_whitespace(String_View s) noexcept {
    u64 start = 0;
    while(start < s.length() && ascii::is_whitespace(s[start])) {
        start++;
    }
    return s.sub(start, s.length());
}

[[nodiscard]] inline Opt<Pair<String_View, String_View>> parse_string(String_View s) noexcept {
    auto result = parse_string_result(s);
    if(!result.ok()) return {};
    return Opt{move(result.unwrap())};
}

[[nodiscard]] inline Result<Pair<String_View, String_View>, String_View> parse_string_result(
    String_View s) noexcept {
    u64 start = 0;
    while(start < s.length() && ascii::is_whitespace(s[start])) {
        start++;
    }
    for(u64 i = start; i < s.length(); i++) {
        if((i + 1 == s.length() && start < i)) {
            return Result<Pair<String_View, String_View>, String_View>::ok(
                Pair{s.sub(start, i + 1), String_View{}});
        }
        if(ascii::is_whitespace(s[i])) {
            return Result<Pair<String_View, String_View>, String_View>::ok(
                Pair{s.sub(start, i), s.sub(i + 1, s.length())});
        }
    }
    return Result<Pair<String_View, String_View>, String_View>::err("parse_string_failed"_v);
}

template<Enum E>
[[nodiscard]] inline Opt<Pair<E, String_View>> parse_enum(String_View s) noexcept {
    auto result = parse_enum_result<E>(s);
    if(!result.ok()) return {};
    return Opt{move(result.unwrap())};
}

template<Enum E>
[[nodiscard]] inline Result<Pair<E, String_View>, String_View> parse_enum_result(
    String_View s) noexcept {
    Result<Pair<E, String_View>, String_View> ret =
        Result<Pair<E, String_View>, String_View>::err("parse_enum_failed"_v);
    if(auto n = parse_string_result(s); n.ok()) {
        auto [name, rest] = spp::move(n.unwrap());
        iterate_enum<E>([&](const Literal& check, const E& check_value) {
            if(name == String_View{check}) {
                ret.emplace_ok(Pair<E, String_View>{check_value, spp::move(rest)});
            }
        });
    }
    return ret;
}

} // namespace Format
} // namespace spp
