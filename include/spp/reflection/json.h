#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

namespace spp::Json {

template<Allocator A>
struct Builder;

template<Allocator A, Reflectable T>
inline void write_json(Builder<A>& b, const T& value) noexcept;
template<Allocator A>
inline void write_json(Builder<A>& b, String_View s) noexcept;

template<Allocator A = Mdefault>
struct Builder {
    Vec<u8, A> out;

    void push(u8 c) noexcept {
        out.push(c);
    }
    void append(String_View s) noexcept {
        for(u8 c : s) out.push(c);
    }
    void append(const char* s) noexcept {
        append(String_View{s});
    }

    [[nodiscard]] String<A> build() noexcept {
        String<A> ret{out.length()};
        ret.set_length(out.length());
        if(out.length() > 0) {
            Libc::memcpy(ret.data(), out.data(), out.length());
        }
        return ret;
    }
};

template<Allocator A>
struct Record_Writer {
    template<Reflectable T>
    void apply(const Literal& name, const T& field) noexcept {
        if(!first) b.push(',');
        first = false;
        write_json(b, String_View{name});
        b.push(':');
        write_json(b, field);
    }

    Builder<A>& b;
    bool first = true;
};

template<Allocator A>
inline void write_json(Builder<A>& b, String_View s) noexcept {
    b.push('"');
    for(u8 c : s) {
        if(c == '"') {
            b.append("\\\"");
        } else if(c == '\\') {
            b.append("\\\\");
        } else if(c == '\n') {
            b.append("\\n");
        } else if(c == '\r') {
            b.append("\\r");
        } else if(c == '\t') {
            b.append("\\t");
        } else {
            b.push(c);
        }
    }
    b.push('"');
}

template<Allocator A, Allocator S>
inline void write_json(Builder<A>& b, const String<S>& s) noexcept {
    write_json(b, s.view());
}

template<Allocator A>
inline void write_json(Builder<A>& b, const char* s) noexcept {
    write_json(b, String_View{s});
}

template<Allocator A, typename T, Allocator V>
inline void write_json(Builder<A>& b, const Vec<T, V>& values) noexcept {
    b.push('[');
    for(u64 i = 0; i < values.length(); i++) {
        if(i != 0) b.push(',');
        write_json(b, values[i]);
    }
    b.push(']');
}

template<Allocator A, typename T>
inline void write_json(Builder<A>& b, const Slice<T>& values) noexcept {
    b.push('[');
    for(u64 i = 0; i < values.length(); i++) {
        if(i != 0) b.push(',');
        write_json(b, values[i]);
    }
    b.push(']');
}

template<Allocator A, typename T, u64 N>
inline void write_json(Builder<A>& b, const Array<T, N>& values) noexcept {
    write_json(b, values.slice());
}

template<typename K>
constexpr bool json_object_key = Same<K, String_View> || Any_String<K>;

template<Allocator A, Key K, Move_Constructable V, Allocator M>
inline void write_json(Builder<A>& b, const Map<K, V, M>& m) noexcept {
    if constexpr(json_object_key<K>) {
        b.push('{');
        bool first = true;
        for(const auto& kv : m) {
            if(!first) b.push(',');
            first = false;
            if constexpr(Same<K, String_View>) {
                write_json(b, kv.first);
            } else {
                write_json(b, kv.first.view());
            }
            b.push(':');
            write_json(b, kv.second);
        }
        b.push('}');
    } else {
        b.push('[');
        bool first = true;
        for(const auto& kv : m) {
            if(!first) b.push(',');
            first = false;
            b.append("{\"key\":");
            write_json(b, kv.first);
            b.append(",\"value\":");
            write_json(b, kv.second);
            b.push('}');
        }
        b.push(']');
    }
}

template<Allocator A, typename T>
inline void write_json(Builder<A>& b, const Opt<T>& v) noexcept {
    if(!v.ok()) {
        b.append("null");
        return;
    }
    write_json(b, *v);
}

template<Allocator A, typename T, typename E>
inline void write_json(Builder<A>& b, const Result<T, E>& r) noexcept {
    if(r.ok()) {
        b.append("{\"ok\":");
        write_json(b, r.unwrap());
        b.push('}');
    } else {
        b.append("{\"err\":");
        write_json(b, r.unwrap_err());
        b.push('}');
    }
}

template<Allocator A, Reflect::Enum E>
inline void write_json_enum(Builder<A>& b, const E& value) noexcept {
    String_View name = "Invalid"_v;
    Reflect::iterate_enum<E>([&](const Literal& check_name, const E& check_value) {
        if(value == check_value) {
            name = String_View{check_name};
        }
    });
    write_json(b, name);
}

template<Allocator A, Reflect::Record R>
inline void write_json_record(Builder<A>& b, const R& value) noexcept {
    b.push('{');
    Record_Writer<A> writer{b, true};
    Reflect::iterate_record(writer, value);
    b.push('}');
}

template<Allocator A, Reflectable T>
inline void write_json(Builder<A>& b, const T& value) noexcept {
    using R = Reflect::Refl<T>;
    if constexpr(R::kind == Reflect::Kind::bool_) {
        b.append(value ? "true" : "false");
    } else if constexpr(R::kind == Reflect::Kind::void_) {
        b.append("null");
    } else if constexpr(R::kind == Reflect::Kind::char_) {
        u8 one[1] = {static_cast<u8>(value)};
        write_json(b, String_View{one, 1});
    } else if constexpr(R::kind == Reflect::Kind::i8_ || R::kind == Reflect::Kind::i16_ ||
                        R::kind == Reflect::Kind::i32_ || R::kind == Reflect::Kind::i64_ ||
                        R::kind == Reflect::Kind::u8_ || R::kind == Reflect::Kind::u16_ ||
                        R::kind == Reflect::Kind::u32_ || R::kind == Reflect::Kind::u64_ ||
                        R::kind == Reflect::Kind::f32_ || R::kind == Reflect::Kind::f64_) {
        auto n = format<Mhidden>("%"_v, value);
        b.append(n.view());
    } else if constexpr(R::kind == Reflect::Kind::array_) {
        b.push('[');
        for(u64 i = 0; i < R::length; i++) {
            if(i != 0) b.push(',');
            write_json(b, value[i]);
        }
        b.push(']');
    } else if constexpr(R::kind == Reflect::Kind::pointer_) {
        if(value == null) {
            b.append("null");
        } else {
            auto p = format<Mhidden>("%"_v, value);
            write_json(b, p.view());
        }
    } else if constexpr(R::kind == Reflect::Kind::enum_) {
        write_json_enum(b, value);
    } else if constexpr(R::kind == Reflect::Kind::record_) {
        write_json_record(b, value);
    } else {
        b.append("null");
    }
}

template<Allocator A = Mdefault, typename T>
[[nodiscard]] inline String<A> stringify(const T& value) noexcept {
    Builder<A> b;
    write_json(b, value);
    return b.build();
}

template<Allocator A = Mdefault>
[[nodiscard]] inline String<A> prettify(String_View minified, u64 indent = 2) noexcept {
    Builder<A> b;
    bool in_string = false;
    bool escaped = false;
    u64 depth = 0;

    auto push_indent = [&](u64 d) {
        for(u64 i = 0; i < d * indent; i++) b.push(' ');
    };

    for(u8 c : minified) {
        if(in_string) {
            b.push(c);
            if(escaped) {
                escaped = false;
            } else if(c == '\\') {
                escaped = true;
            } else if(c == '"') {
                in_string = false;
            }
            continue;
        }

        if(c == '"') {
            in_string = true;
            b.push(c);
            continue;
        }
        if(c == '{' || c == '[') {
            b.push(c);
            b.push('\n');
            depth++;
            push_indent(depth);
            continue;
        }
        if(c == '}' || c == ']') {
            b.push('\n');
            if(depth > 0) depth--;
            push_indent(depth);
            b.push(c);
            continue;
        }
        if(c == ',') {
            b.push(c);
            b.push('\n');
            push_indent(depth);
            continue;
        }
        if(c == ':') {
            b.push(':');
            b.push(' ');
            continue;
        }
        b.push(c);
    }

    return b.build();
}

template<Allocator A = Mdefault, typename T>
[[nodiscard]] inline String<A> stringify_pretty(const T& value, u64 indent = 2) noexcept {
    auto compact = stringify<A>(value);
    return prettify<A>(compact.view(), indent);
}

namespace detail {

struct Cursor {
    String_View s;
    u64 i = 0;
};

inline void skip_ws(Cursor& c) noexcept {
    while(c.i < c.s.length() && ascii::is_whitespace(c.s[c.i])) c.i++;
}

[[nodiscard]] inline Result<String<Mdefault>, String_View> parse_string(Cursor& c) noexcept {
    skip_ws(c);
    if(c.i >= c.s.length() || c.s[c.i] != '"') {
        return Result<String<Mdefault>, String_View>::err("json_expected_string"_v);
    }
    c.i++;

    Vec<u8> out;
    while(c.i < c.s.length()) {
        u8 ch = c.s[c.i++];
        if(ch == '"') {
            auto ret = String<Mdefault>{out.length()};
            ret.set_length(out.length());
            if(out.length()) Libc::memcpy(ret.data(), out.data(), out.length());
            return Result<String<Mdefault>, String_View>::ok(spp::move(ret));
        }
        if(ch == '\\') {
            if(c.i >= c.s.length()) {
                return Result<String<Mdefault>, String_View>::err("json_bad_escape"_v);
            }
            u8 esc = c.s[c.i++];
            if(esc == '"' || esc == '\\' || esc == '/') out.push(esc);
            else if(esc == 'n') out.push('\n');
            else if(esc == 'r') out.push('\r');
            else if(esc == 't') out.push('\t');
            else return Result<String<Mdefault>, String_View>::err("json_bad_escape"_v);
        } else {
            out.push(ch);
        }
    }
    return Result<String<Mdefault>, String_View>::err("json_unterminated_string"_v);
}

template<typename T>
[[nodiscard]] inline Result<T, String_View> parse_value(Cursor& c) noexcept;

template<typename T, Allocator A>
[[nodiscard]] inline Result<Vec<T, A>, String_View> parse_array(Cursor& c) noexcept {
    skip_ws(c);
    if(c.i >= c.s.length() || c.s[c.i] != '[') {
        return Result<Vec<T, A>, String_View>::err("json_expected_array"_v);
    }
    c.i++;
    Vec<T, A> values;
    skip_ws(c);
    if(c.i < c.s.length() && c.s[c.i] == ']') {
        c.i++;
        return Result<Vec<T, A>, String_View>::ok(spp::move(values));
    }

    for(;;) {
        auto item = parse_value<T>(c);
        if(!item.ok()) return Result<Vec<T, A>, String_View>::err(spp::move(item.unwrap_err()));
        values.push(spp::move(item.unwrap()));
        skip_ws(c);
        if(c.i >= c.s.length()) {
            return Result<Vec<T, A>, String_View>::err("json_unterminated_array"_v);
        }
        if(c.s[c.i] == ']') {
            c.i++;
            return Result<Vec<T, A>, String_View>::ok(spp::move(values));
        }
        if(c.s[c.i] != ',') {
            return Result<Vec<T, A>, String_View>::err("json_expected_comma"_v);
        }
        c.i++;
    }
}

template<typename T>
[[nodiscard]] inline Result<T, String_View> parse_value(Cursor& c) noexcept {
    skip_ws(c);

    if constexpr(Same<T, bool>) {
        if(c.i + 4 <= c.s.length() && c.s.sub(c.i, c.i + 4) == "true"_v) {
            c.i += 4;
            return Result<T, String_View>::ok(true);
        }
        if(c.i + 5 <= c.s.length() && c.s.sub(c.i, c.i + 5) == "false"_v) {
            c.i += 5;
            return Result<T, String_View>::ok(false);
        }
        return Result<T, String_View>::err("json_expected_bool"_v);
    } else if constexpr(Same<T, i64>) {
        auto parsed = Format::parse_i64_result(c.s.sub(c.i, c.s.length()));
        if(!parsed.ok()) return Result<T, String_View>::err("json_expected_i64"_v);
        auto [value, rest] = parsed.unwrap();
        c.i = c.s.length() - rest.length();
        return Result<T, String_View>::ok(spp::move(value));
    } else if constexpr(Same<T, i32>) {
        auto parsed = parse_value<i64>(c);
        if(!parsed.ok()) return Result<T, String_View>::err(spp::move(parsed.unwrap_err()));
        i64 v = parsed.unwrap();
        return Result<T, String_View>::ok(static_cast<i32>(v));
    } else if constexpr(Same<T, f64>) {
        auto parsed = Format::parse_f32_result(c.s.sub(c.i, c.s.length()));
        if(!parsed.ok()) return Result<T, String_View>::err("json_expected_f64"_v);
        auto [value, rest] = parsed.unwrap();
        c.i = c.s.length() - rest.length();
        return Result<T, String_View>::ok(static_cast<f64>(value));
    } else if constexpr(Same<T, f32>) {
        auto parsed = Format::parse_f32_result(c.s.sub(c.i, c.s.length()));
        if(!parsed.ok()) return Result<T, String_View>::err("json_expected_f32"_v);
        auto [value, rest] = parsed.unwrap();
        c.i = c.s.length() - rest.length();
        return Result<T, String_View>::ok(spp::move(value));
    } else if constexpr(Any_String<T>) {
        auto parsed = parse_string(c);
        if(!parsed.ok()) return Result<T, String_View>::err(spp::move(parsed.unwrap_err()));
        if constexpr(Same<T, String<Mdefault>>) {
            return Result<T, String_View>::ok(spp::move(parsed.unwrap()));
        } else {
            T value = spp::move(parsed.unwrap());
            return Result<T, String_View>::ok(spp::move(value));
        }
    } else {
        return Result<T, String_View>::err("json_parse_unsupported_type"_v);
    }
}

} // namespace detail

template<typename T>
[[nodiscard]] inline Result<T, String_View> parse_result(String_View input) noexcept {
    detail::Cursor c{input, 0};
    auto parsed = detail::parse_value<T>(c);
    if(!parsed.ok()) return Result<T, String_View>::err(spp::move(parsed.unwrap_err()));
    detail::skip_ws(c);
    if(c.i != input.length()) {
        return Result<T, String_View>::err("json_trailing_chars"_v);
    }
    return spp::move(parsed);
}

template<typename T, Allocator A = Mdefault>
[[nodiscard]] inline Result<Vec<T, A>, String_View> parse_vec_result(String_View input) noexcept {
    detail::Cursor c{input, 0};
    auto parsed = detail::parse_array<T, A>(c);
    if(!parsed.ok()) return Result<Vec<T, A>, String_View>::err(spp::move(parsed.unwrap_err()));
    detail::skip_ws(c);
    if(c.i != input.length()) {
        return Result<Vec<T, A>, String_View>::err("json_trailing_chars"_v);
    }
    return spp::move(parsed);
}

} // namespace spp::Json
