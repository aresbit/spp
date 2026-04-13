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

} // namespace spp::Json
