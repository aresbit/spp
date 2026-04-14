#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

#include <spp/io/files.h>

namespace spp::Binary {

constexpr u8 k_magic0 = 'S';
constexpr u8 k_magic1 = 'P';
constexpr u8 k_magic2 = 'P';
constexpr u8 k_magic3 = 'B';
constexpr u16 k_protocol_version = 1;

enum struct Wire_Kind : u8 {
    i8 = 1,
    i16 = 2,
    i32 = 3,
    i64 = 4,
    u8 = 5,
    u16 = 6,
    u32 = 7,
    u64 = 8,
    f32 = 9,
    f64 = 10,
    bool_ = 11,
    char_ = 12,
    enum_ = 13,
    string = 14,
    array = 15,
    vec = 16,
    opt = 17,
    result = 18,
    record = 19,
};

struct Header {
    u16 protocol = k_protocol_version;
    u16 schema = 1;
    u64 type_id = 0;
    u32 payload_size = 0;
};

namespace detail {

template<typename T>
struct Is_String {
    constexpr static bool value = false;
};
template<Allocator A>
struct Is_String<String<A>> {
    constexpr static bool value = true;
};
template<typename T>
constexpr bool is_string = Is_String<Decay<T>>::value;

template<typename T>
struct Is_Vec {
    constexpr static bool value = false;
};
template<typename T, Allocator A>
struct Is_Vec<Vec<T, A>> {
    constexpr static bool value = true;
};
template<typename T>
constexpr bool is_vec = Is_Vec<Decay<T>>::value;

template<typename T>
struct Vec_Elem;
template<typename T, Allocator A>
struct Vec_Elem<Vec<T, A>> {
    using type = T;
};

template<typename T>
struct Is_Opt {
    constexpr static bool value = false;
};
template<typename T>
struct Is_Opt<Opt<T>> {
    constexpr static bool value = true;
};
template<typename T>
constexpr bool is_opt = Is_Opt<Decay<T>>::value;

template<typename T>
struct Opt_Elem;
template<typename T>
struct Opt_Elem<Opt<T>> {
    using type = T;
};

template<typename T>
struct Is_Result {
    constexpr static bool value = false;
};
template<typename T, typename E>
struct Is_Result<Result<T, E>> {
    constexpr static bool value = true;
};
template<typename T>
constexpr bool is_result = Is_Result<Decay<T>>::value;

template<typename T>
struct String_Alloc;
template<Allocator A>
struct String_Alloc<String<A>> {
    using type = A;
};

template<typename T>
struct Result_Ok;
template<typename T, typename E>
struct Result_Ok<Result<T, E>> {
    using type = T;
};

template<typename T>
struct Result_Err;
template<typename T, typename E>
struct Result_Err<Result<T, E>> {
    using type = E;
};

[[nodiscard]] inline u64 hash_bytes(String_View bytes) noexcept {
    u64 h = 1469598103934665603ull;
    for(u8 c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

template<Reflectable T>
[[nodiscard]] inline u64 type_id() noexcept {
    return hash_bytes(String_View{Reflect::Refl<T>::name});
}

[[nodiscard]] inline u64 field_id(String_View field_name) noexcept {
    return hash_bytes(field_name);
}

template<Allocator A = Mdefault>
struct Writer {
    Vec<u8, A> out;

    void push_u8(u8 v) noexcept {
        out.push(v);
    }

    void push_u16(u16 v) noexcept {
        push_u8(static_cast<u8>(v & 0xff));
        push_u8(static_cast<u8>((v >> 8) & 0xff));
    }

    void push_u32(u32 v) noexcept {
        for(u64 i = 0; i < 4; i++) {
            push_u8(static_cast<u8>((v >> (8 * i)) & 0xff));
        }
    }

    void push_u64(u64 v) noexcept {
        for(u64 i = 0; i < 8; i++) {
            push_u8(static_cast<u8>((v >> (8 * i)) & 0xff));
        }
    }

    void push_i8(i8 v) noexcept {
        push_u8(static_cast<u8>(v));
    }
    void push_i16(i16 v) noexcept {
        push_u16(static_cast<u16>(v));
    }
    void push_i32(i32 v) noexcept {
        push_u32(static_cast<u32>(v));
    }
    void push_i64(i64 v) noexcept {
        push_u64(static_cast<u64>(v));
    }

    void push_f32(f32 v) noexcept {
        u32 raw = *reinterpret_cast<u32*>(&v);
        push_u32(raw);
    }
    void push_f64(f64 v) noexcept {
        u64 raw = *reinterpret_cast<u64*>(&v);
        push_u64(raw);
    }

    void append(Slice<const u8> bytes) noexcept {
        for(u8 c : bytes) out.push(c);
    }
};

struct Reader {
    Slice<const u8> in;
    u64 i = 0;

    [[nodiscard]] bool can(u64 n) const noexcept {
        return i + n <= in.length();
    }

    [[nodiscard]] Result<u8, String_View> read_u8() noexcept {
        if(!can(1)) return Result<u8, String_View>::err("binary_eof"_v);
        u8 v = in[i++];
        return Result<u8, String_View>::ok(spp::move(v));
    }

    [[nodiscard]] Result<u16, String_View> read_u16() noexcept {
        if(!can(2)) return Result<u16, String_View>::err("binary_eof"_v);
        u16 v = static_cast<u16>(in[i]) | (static_cast<u16>(in[i + 1]) << 8);
        i += 2;
        return Result<u16, String_View>::ok(spp::move(v));
    }

    [[nodiscard]] Result<u32, String_View> read_u32() noexcept {
        if(!can(4)) return Result<u32, String_View>::err("binary_eof"_v);
        u32 v = 0;
        for(u64 k = 0; k < 4; k++) {
            v |= static_cast<u32>(in[i + k]) << (8 * k);
        }
        i += 4;
        return Result<u32, String_View>::ok(spp::move(v));
    }

    [[nodiscard]] Result<u64, String_View> read_u64() noexcept {
        if(!can(8)) return Result<u64, String_View>::err("binary_eof"_v);
        u64 v = 0;
        for(u64 k = 0; k < 8; k++) {
            v |= static_cast<u64>(in[i + k]) << (8 * k);
        }
        i += 8;
        return Result<u64, String_View>::ok(spp::move(v));
    }

    [[nodiscard]] Result<i8, String_View> read_i8() noexcept {
        auto v = read_u8();
        if(!v.ok()) return Result<i8, String_View>::err(spp::move(v.unwrap_err()));
        i8 out = static_cast<i8>(v.unwrap());
        return Result<i8, String_View>::ok(spp::move(out));
    }

    [[nodiscard]] Result<i16, String_View> read_i16() noexcept {
        auto v = read_u16();
        if(!v.ok()) return Result<i16, String_View>::err(spp::move(v.unwrap_err()));
        i16 out = static_cast<i16>(v.unwrap());
        return Result<i16, String_View>::ok(spp::move(out));
    }

    [[nodiscard]] Result<i32, String_View> read_i32() noexcept {
        auto v = read_u32();
        if(!v.ok()) return Result<i32, String_View>::err(spp::move(v.unwrap_err()));
        i32 out = static_cast<i32>(v.unwrap());
        return Result<i32, String_View>::ok(spp::move(out));
    }

    [[nodiscard]] Result<i64, String_View> read_i64() noexcept {
        auto v = read_u64();
        if(!v.ok()) return Result<i64, String_View>::err(spp::move(v.unwrap_err()));
        i64 out = static_cast<i64>(v.unwrap());
        return Result<i64, String_View>::ok(spp::move(out));
    }

    [[nodiscard]] Result<f32, String_View> read_f32() noexcept {
        auto v = read_u32();
        if(!v.ok()) return Result<f32, String_View>::err(spp::move(v.unwrap_err()));
        f32 out = *reinterpret_cast<f32*>(&v.unwrap());
        return Result<f32, String_View>::ok(spp::move(out));
    }

    [[nodiscard]] Result<f64, String_View> read_f64() noexcept {
        auto v = read_u64();
        if(!v.ok()) return Result<f64, String_View>::err(spp::move(v.unwrap_err()));
        f64 out = *reinterpret_cast<f64*>(&v.unwrap());
        return Result<f64, String_View>::ok(spp::move(out));
    }

    [[nodiscard]] Result<Slice<const u8>, String_View> read_bytes(u64 n) noexcept {
        if(!can(n)) return Result<Slice<const u8>, String_View>::err("binary_eof"_v);
        Slice<const u8> s{in.data() + i, n};
        i += n;
        return Result<Slice<const u8>, String_View>::ok(spp::move(s));
    }
};

template<Reflectable T, Allocator A>
Result<u64, String_View> encode_value(Writer<A>& w, const T& value) noexcept;

template<Reflectable T>
Result<T, String_View> decode_value(Reader& r) noexcept;

struct Record_Field_Counter {
    template<typename F>
    void apply(const Literal&, const F&) noexcept {
        count++;
    }
    u32 count = 0;
};

template<Allocator A>
struct Record_Field_Encoder {
    template<typename F>
    void apply(const Literal& name, const F& field) noexcept {
        Writer<A> local;
        auto enc = encode_value(local, field);
        if(!enc.ok()) {
            ok = false;
            err = spp::move(enc.unwrap_err());
            return;
        }
        if(local.out.length() > SPP_UINT32_MAX) {
            ok = false;
            err = "binary_field_too_large"_v;
            return;
        }
        w->push_u64(field_id(String_View{name}));
        w->push_u32(static_cast<u32>(local.out.length()));
        w->append(local.out.slice());
    }
    Writer<A>* w = null;
    bool ok = true;
    String_View err;
};

template<typename R>
struct Record_Field_Decoder {
    template<typename F>
    void apply(const Literal& name, F& field) noexcept {
        if(matched || field_id(String_View{name}) != want_id) return;
        using V = Decay<F>;
        auto got = decode_value<V>(*reader);
        if(!got.ok()) {
            decode_ok = false;
            decode_err = spp::move(got.unwrap_err());
            return;
        }
        field = spp::move(got.unwrap());
        matched = true;
    }

    Reader* reader = null;
    u64 want_id = 0;
    bool matched = false;
    bool decode_ok = true;
    String_View decode_err;
};

template<Reflect::Record R, Allocator A>
Result<u64, String_View> encode_record_value(Writer<A>& w, const R& value) noexcept {
    Record_Field_Counter c;
    Reflect::iterate_record(c, value);

    u64 start = w.out.length();
    w.push_u8(static_cast<u8>(Wire_Kind::record));
    w.push_u32(c.count);

    Record_Field_Encoder<A> fe{&w};
    Reflect::iterate_record(fe, value);
    if(!fe.ok) return Result<u64, String_View>::err(spp::move(fe.err));
    u64 written = w.out.length() - start;
    return Result<u64, String_View>::ok(spp::move(written));
}

template<Reflect::Record R>
Result<R, String_View> decode_record_value(Reader& r) noexcept
    requires Default_Constructable<R>
{
    auto count = r.read_u32();
    if(!count.ok()) return Result<R, String_View>::err(spp::move(count.unwrap_err()));

    R out{};
    for(u32 n = 0; n < count.unwrap(); n++) {
        auto fid = r.read_u64();
        if(!fid.ok()) return Result<R, String_View>::err(spp::move(fid.unwrap_err()));
        auto flen = r.read_u32();
        if(!flen.ok()) return Result<R, String_View>::err(spp::move(flen.unwrap_err()));
        auto fbytes = r.read_bytes(flen.unwrap());
        if(!fbytes.ok()) return Result<R, String_View>::err(spp::move(fbytes.unwrap_err()));

        Reader fr{fbytes.unwrap(), 0};
        Record_Field_Decoder<R> fd{&fr, fid.unwrap()};
        Reflect::iterate_record(fd, out);
        if(!fd.decode_ok) {
            return Result<R, String_View>::err(spp::move(fd.decode_err));
        }
        if(fd.matched && fr.i != fr.in.length()) {
            return Result<R, String_View>::err("binary_field_trailing_bytes"_v);
        }
    }
    return Result<R, String_View>::ok(spp::move(out));
}

template<Reflectable T, Allocator A>
Result<u64, String_View> encode_value(Writer<A>& w, const T& value) noexcept {
    using R = Reflect::Refl<T>;

    if constexpr(R::kind == Reflect::Kind::i8_) {
        w.push_u8(static_cast<u8>(Wire_Kind::i8));
        w.push_i8(value);
    } else if constexpr(R::kind == Reflect::Kind::i16_) {
        w.push_u8(static_cast<u8>(Wire_Kind::i16));
        w.push_i16(value);
    } else if constexpr(R::kind == Reflect::Kind::i32_) {
        w.push_u8(static_cast<u8>(Wire_Kind::i32));
        w.push_i32(value);
    } else if constexpr(R::kind == Reflect::Kind::i64_) {
        w.push_u8(static_cast<u8>(Wire_Kind::i64));
        w.push_i64(value);
    } else if constexpr(R::kind == Reflect::Kind::u8_) {
        w.push_u8(static_cast<u8>(Wire_Kind::u8));
        w.push_u8(value);
    } else if constexpr(R::kind == Reflect::Kind::u16_) {
        w.push_u8(static_cast<u8>(Wire_Kind::u16));
        w.push_u16(value);
    } else if constexpr(R::kind == Reflect::Kind::u32_) {
        w.push_u8(static_cast<u8>(Wire_Kind::u32));
        w.push_u32(value);
    } else if constexpr(R::kind == Reflect::Kind::u64_) {
        w.push_u8(static_cast<u8>(Wire_Kind::u64));
        w.push_u64(value);
    } else if constexpr(R::kind == Reflect::Kind::f32_) {
        w.push_u8(static_cast<u8>(Wire_Kind::f32));
        w.push_f32(value);
    } else if constexpr(R::kind == Reflect::Kind::f64_) {
        w.push_u8(static_cast<u8>(Wire_Kind::f64));
        w.push_f64(value);
    } else if constexpr(R::kind == Reflect::Kind::bool_) {
        w.push_u8(static_cast<u8>(Wire_Kind::bool_));
        w.push_u8(value ? 1 : 0);
    } else if constexpr(R::kind == Reflect::Kind::char_) {
        w.push_u8(static_cast<u8>(Wire_Kind::char_));
        w.push_u8(static_cast<u8>(value));
    } else if constexpr(R::kind == Reflect::Kind::enum_) {
        w.push_u8(static_cast<u8>(Wire_Kind::enum_));
        using U = typename R::underlying;
        if constexpr(Unsigned_Int<U>) {
            w.push_u64(static_cast<u64>(static_cast<U>(value)));
        } else {
            w.push_i64(static_cast<i64>(static_cast<U>(value)));
        }
    } else if constexpr(is_string<T>) {
        w.push_u8(static_cast<u8>(Wire_Kind::string));
        String_View sv = value.view();
        w.push_u32(static_cast<u32>(sv.length()));
        w.append(Slice<const u8>{sv.data(), sv.length()});
    } else if constexpr(Same<Decay<T>, String_View>) {
        w.push_u8(static_cast<u8>(Wire_Kind::string));
        w.push_u32(static_cast<u32>(value.length()));
        w.append(Slice<const u8>{value.data(), value.length()});
    } else if constexpr(R::kind == Reflect::Kind::array_) {
        w.push_u8(static_cast<u8>(Wire_Kind::array));
        w.push_u32(static_cast<u32>(R::length));
        for(u64 i = 0; i < R::length; i++) {
            auto enc = encode_value(w, value[i]);
            if(!enc.ok()) return Result<u64, String_View>::err(spp::move(enc.unwrap_err()));
        }
    } else if constexpr(is_vec<T>) {
        w.push_u8(static_cast<u8>(Wire_Kind::vec));
        w.push_u32(static_cast<u32>(value.length()));
        for(const auto& e : value) {
            auto enc = encode_value(w, e);
            if(!enc.ok()) return Result<u64, String_View>::err(spp::move(enc.unwrap_err()));
        }
    } else if constexpr(is_opt<T>) {
        w.push_u8(static_cast<u8>(Wire_Kind::opt));
        w.push_u8(value.ok() ? 1 : 0);
        if(value.ok()) {
            auto enc = encode_value(w, *value);
            if(!enc.ok()) return Result<u64, String_View>::err(spp::move(enc.unwrap_err()));
        }
    } else if constexpr(is_result<T>) {
        w.push_u8(static_cast<u8>(Wire_Kind::result));
        w.push_u8(value.ok() ? 1 : 0);
        if(value.ok()) {
            auto enc = encode_value(w, value.unwrap());
            if(!enc.ok()) return Result<u64, String_View>::err(spp::move(enc.unwrap_err()));
        } else {
            auto enc = encode_value(w, value.unwrap_err());
            if(!enc.ok()) return Result<u64, String_View>::err(spp::move(enc.unwrap_err()));
        }
    } else if constexpr(Reflect::Record<T>) {
        return encode_record_value(w, value);
    } else {
        return Result<u64, String_View>::err("binary_unsupported_type"_v);
    }
    return Result<u64, String_View>::ok(1);
}

template<Reflectable T>
Result<T, String_View> decode_value(Reader& r) noexcept {
    using R = Reflect::Refl<T>;
    auto kind = r.read_u8();
    if(!kind.ok()) return Result<T, String_View>::err(spp::move(kind.unwrap_err()));

    if constexpr(R::kind == Reflect::Kind::i8_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::i8))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_i8();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::i16_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::i16))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_i16();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::i32_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::i32))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_i32();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::i64_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::i64))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_i64();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::u8_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::u8))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_u8();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::u16_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::u16))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_u16();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::u32_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::u32))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_u32();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::u64_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::u64))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_u64();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::f32_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::f32))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_f32();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::f64_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::f64))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_f64();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(v.unwrap()));
    } else if constexpr(R::kind == Reflect::Kind::bool_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::bool_))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_u8();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(v.unwrap() != 0);
    } else if constexpr(R::kind == Reflect::Kind::char_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::char_))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto v = r.read_u8();
        if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(static_cast<char>(v.unwrap())));
    } else if constexpr(R::kind == Reflect::Kind::enum_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::enum_))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        using U = typename R::underlying;
        if constexpr(Unsigned_Int<U>) {
            auto v = r.read_u64();
            if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
            return Result<T, String_View>::ok(spp::move(static_cast<T>(static_cast<U>(v.unwrap()))));
        } else {
            auto v = r.read_i64();
            if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
            return Result<T, String_View>::ok(spp::move(static_cast<T>(static_cast<U>(v.unwrap()))));
        }
    } else if constexpr(is_string<T>) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::string))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto n = r.read_u32();
        if(!n.ok()) return Result<T, String_View>::err(spp::move(n.unwrap_err()));
        auto b = r.read_bytes(n.unwrap());
        if(!b.ok()) return Result<T, String_View>::err(spp::move(b.unwrap_err()));
        String_View sv{b.unwrap().data(), b.unwrap().length()};
        return Result<T, String_View>::ok(
            sv.template string<typename String_Alloc<Decay<T>>::type>());
    } else if constexpr(Same<Decay<T>, String_View>) {
        return Result<T, String_View>::err("binary_decode_string_view_unsupported"_v);
    } else if constexpr(R::kind == Reflect::Kind::array_) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::array))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto n = r.read_u32();
        if(!n.ok()) return Result<T, String_View>::err(spp::move(n.unwrap_err()));
        if(n.unwrap() != R::length) return Result<T, String_View>::err("binary_array_len_mismatch"_v);
        T out{};
        for(u64 i = 0; i < R::length; i++) {
            auto v = decode_value<typename R::underlying>(r);
            if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
            out[i] = spp::move(v.unwrap());
        }
        return Result<T, String_View>::ok(spp::move(out));
    } else if constexpr(is_vec<T>) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::vec))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto n = r.read_u32();
        if(!n.ok()) return Result<T, String_View>::err(spp::move(n.unwrap_err()));
        T out{};
        out.reserve(n.unwrap());
        for(u32 i = 0; i < n.unwrap(); i++) {
            auto v = decode_value<typename Vec_Elem<T>::type>(r);
            if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
            out.push(spp::move(v.unwrap()));
        }
        return Result<T, String_View>::ok(spp::move(out));
    } else if constexpr(is_opt<T>) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::opt))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto has = r.read_u8();
        if(!has.ok()) return Result<T, String_View>::err(spp::move(has.unwrap_err()));
        T out{};
        if(has.unwrap()) {
            auto v = decode_value<typename Opt_Elem<T>::type>(r);
            if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
            out.emplace(spp::move(v.unwrap()));
        }
        return Result<T, String_View>::ok(spp::move(out));
    } else if constexpr(is_result<T>) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::result))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        auto ok = r.read_u8();
        if(!ok.ok()) return Result<T, String_View>::err(spp::move(ok.unwrap_err()));
        if(ok.unwrap()) {
            auto v = decode_value<typename Result_Ok<T>::type>(r);
            if(!v.ok()) return Result<T, String_View>::err(spp::move(v.unwrap_err()));
            return Result<T, String_View>::ok(spp::move(T::ok(spp::move(v.unwrap()))));
        }
        auto e = decode_value<typename Result_Err<T>::type>(r);
        if(!e.ok()) return Result<T, String_View>::err(spp::move(e.unwrap_err()));
        return Result<T, String_View>::ok(spp::move(T::err(spp::move(e.unwrap()))));
    } else if constexpr(Reflect::Record<T>) {
        if(kind.unwrap() != static_cast<u8>(Wire_Kind::record))
            return Result<T, String_View>::err("binary_kind_mismatch"_v);
        if constexpr(Default_Constructable<T>) {
            return decode_record_value<T>(r);
        } else {
            return Result<T, String_View>::err("binary_record_default_ctor_required"_v);
        }
    } else {
        return Result<T, String_View>::err("binary_unsupported_type"_v);
    }
}

} // namespace detail

template<Allocator A = Mdefault, Reflectable T>
[[nodiscard]] Result<Vec<u8, A>, String_View> encode_result(const T& value,
                                                            u16 schema_version = 1) noexcept {
    detail::Writer<A> payload;
    auto enc = detail::encode_value(payload, value);
    if(!enc.ok()) return Result<Vec<u8, A>, String_View>::err(spp::move(enc.unwrap_err()));

    if(payload.out.length() > SPP_UINT32_MAX) {
        return Result<Vec<u8, A>, String_View>::err("binary_payload_too_large"_v);
    }

    detail::Writer<A> out;
    out.push_u8(k_magic0);
    out.push_u8(k_magic1);
    out.push_u8(k_magic2);
    out.push_u8(k_magic3);
    out.push_u16(k_protocol_version);
    out.push_u16(schema_version);
    out.push_u64(detail::type_id<T>());
    out.push_u32(static_cast<u32>(payload.out.length()));
    out.append(payload.out.slice());
    return Result<Vec<u8, A>, String_View>::ok(spp::move(out.out));
}

template<Reflectable T>
[[nodiscard]] Result<Pair<T, Header>, String_View>
decode_with_header_result(Slice<const u8> bytes) noexcept {
    detail::Reader r{bytes, 0};

    auto m0 = r.read_u8();
    auto m1 = r.read_u8();
    auto m2 = r.read_u8();
    auto m3 = r.read_u8();
    if(!m0.ok() || !m1.ok() || !m2.ok() || !m3.ok()) {
        return Result<Pair<T, Header>, String_View>::err("binary_bad_header"_v);
    }
    if(m0.unwrap() != k_magic0 || m1.unwrap() != k_magic1 || m2.unwrap() != k_magic2 ||
       m3.unwrap() != k_magic3) {
        return Result<Pair<T, Header>, String_View>::err("binary_bad_magic"_v);
    }

    Header h;
    auto pv = r.read_u16();
    if(!pv.ok()) return Result<Pair<T, Header>, String_View>::err(spp::move(pv.unwrap_err()));
    h.protocol = pv.unwrap();
    if(h.protocol != k_protocol_version) {
        return Result<Pair<T, Header>, String_View>::err("binary_protocol_mismatch"_v);
    }

    auto sv = r.read_u16();
    auto tid = r.read_u64();
    auto psz = r.read_u32();
    if(!sv.ok() || !tid.ok() || !psz.ok()) {
        return Result<Pair<T, Header>, String_View>::err("binary_bad_header"_v);
    }
    h.schema = sv.unwrap();
    h.type_id = tid.unwrap();
    h.payload_size = psz.unwrap();

    if(h.type_id != detail::type_id<T>()) {
        return Result<Pair<T, Header>, String_View>::err("binary_type_mismatch"_v);
    }

    auto body = r.read_bytes(h.payload_size);
    if(!body.ok()) return Result<Pair<T, Header>, String_View>::err(spp::move(body.unwrap_err()));
    if(r.i != bytes.length()) {
        return Result<Pair<T, Header>, String_View>::err("binary_trailing_bytes"_v);
    }

    detail::Reader payload{body.unwrap(), 0};
    auto decoded = detail::decode_value<T>(payload);
    if(!decoded.ok()) return Result<Pair<T, Header>, String_View>::err(spp::move(decoded.unwrap_err()));
    if(payload.i != payload.in.length()) {
        return Result<Pair<T, Header>, String_View>::err("binary_payload_trailing_bytes"_v);
    }

    Pair<T, Header> out{spp::move(decoded.unwrap()), h};
    return Result<Pair<T, Header>, String_View>::ok(spp::move(out));
}

template<Reflectable T>
[[nodiscard]] Result<T, String_View> decode_result(Slice<const u8> bytes) noexcept {
    auto d = decode_with_header_result<T>(bytes);
    if(!d.ok()) return Result<T, String_View>::err(spp::move(d.unwrap_err()));
    return Result<T, String_View>::ok(spp::move(d.unwrap().first));
}

template<Allocator A = Mdefault, Reflectable T>
[[nodiscard]] Result<u64, String_View> persist_result(String_View path, const T& value,
                                                      u16 schema_version = 1) noexcept {
    auto encoded = encode_result<A>(value, schema_version);
    if(!encoded.ok()) return Result<u64, String_View>::err(spp::move(encoded.unwrap_err()));
    auto wrote = Files::write_result(path, encoded.unwrap().slice());
    if(!wrote.ok()) return Result<u64, String_View>::err(spp::move(wrote.unwrap_err()));
    return Result<u64, String_View>::ok(spp::move(wrote.unwrap()));
}

template<Reflectable T>
[[nodiscard]] Result<Pair<T, Header>, String_View>
load_with_header_result(String_View path) noexcept {
    auto bytes = Files::read_result(path);
    if(!bytes.ok()) return Result<Pair<T, Header>, String_View>::err(spp::move(bytes.unwrap_err()));
    return decode_with_header_result<T>(bytes.unwrap().slice());
}

template<Reflectable T>
[[nodiscard]] Result<T, String_View> load_result(String_View path) noexcept {
    auto loaded = load_with_header_result<T>(path);
    if(!loaded.ok()) return Result<T, String_View>::err(spp::move(loaded.unwrap_err()));
    return Result<T, String_View>::ok(spp::move(loaded.unwrap().first));
}

} // namespace spp::Binary
