#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

namespace spp {

template<u32 Scale = 6>
struct Decimal {
    static_assert(Scale <= 18);

    constexpr Decimal() noexcept = default;
    explicit constexpr Decimal(i64 raw) noexcept : raw_(raw) {
    }

    [[nodiscard]] static constexpr Decimal from_raw(i64 raw) noexcept {
        return Decimal{raw};
    }

    [[nodiscard]] static constexpr Decimal from_int(i64 whole) noexcept {
        return Decimal{whole * static_cast<i64>(factor())};
    }

    [[nodiscard]] static constexpr u32 scale() noexcept {
        return Scale;
    }

    [[nodiscard]] static constexpr u64 factor() noexcept {
        u64 out = 1;
        for(u32 i = 0; i < Scale; i++) {
            out *= 10;
        }
        return out;
    }

    [[nodiscard]] constexpr i64 raw() const noexcept {
        return raw_;
    }

    [[nodiscard]] constexpr i64 whole() const noexcept {
        return raw_ / static_cast<i64>(factor());
    }

    [[nodiscard]] constexpr u64 frac() const noexcept {
        i64 f = raw_ % static_cast<i64>(factor());
        if(f < 0) f = -f;
        return static_cast<u64>(f);
    }

    [[nodiscard]] static Result<Decimal, String_View> from_ratio_result(i64 numerator,
                                                                        i64 denominator) noexcept {
        if(denominator == 0) return Result<Decimal, String_View>::err("division_by_zero"_v);

        i64 scaled = 0;
        if(!checked_mul_(numerator, static_cast<i64>(factor()), scaled)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        return Result<Decimal, String_View>::ok(Decimal{scaled / denominator});
    }

    [[nodiscard]] Result<Decimal, String_View> add_result(const Decimal& rhs) const noexcept {
        i64 out = 0;
        if(!checked_add_(raw_, rhs.raw_, out)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        return Result<Decimal, String_View>::ok(Decimal{out});
    }

    [[nodiscard]] Result<Decimal, String_View> sub_result(const Decimal& rhs) const noexcept {
        i64 out = 0;
        if(!checked_sub_(raw_, rhs.raw_, out)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        return Result<Decimal, String_View>::ok(Decimal{out});
    }

    [[nodiscard]] Result<Decimal, String_View> mul_result(const Decimal& rhs) const noexcept {
        i64 div = static_cast<i64>(factor());
        i64 ah = raw_ / div;
        i64 al = raw_ % div;

        i64 p0 = 0;
        i64 p1 = 0;
        if(!checked_mul_(ah, rhs.raw_, p0)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        if(!checked_mul_(al, rhs.raw_, p1)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        p1 /= div;

        i64 out = 0;
        if(!checked_add_(p0, p1, out)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        return Result<Decimal, String_View>::ok(Decimal{out});
    }

    [[nodiscard]] Result<Decimal, String_View> div_result(const Decimal& rhs) const noexcept {
        if(rhs.raw_ == 0) return Result<Decimal, String_View>::err("division_by_zero"_v);

        i64 mul = static_cast<i64>(factor());
        i64 q = raw_ / rhs.raw_;
        i64 r = raw_ % rhs.raw_;

        i64 p0 = 0;
        i64 p1 = 0;
        if(!checked_mul_(q, mul, p0)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        if(!checked_mul_(r, mul, p1)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        p1 /= rhs.raw_;

        i64 out = 0;
        if(!checked_add_(p0, p1, out)) {
            return Result<Decimal, String_View>::err("overflow"_v);
        }
        return Result<Decimal, String_View>::ok(Decimal{out});
    }

    [[nodiscard]] Decimal operator+(const Decimal& rhs) const noexcept {
        auto ret = add_result(rhs);
        assert(ret.ok());
        return ret.unwrap();
    }
    [[nodiscard]] Decimal operator-(const Decimal& rhs) const noexcept {
        auto ret = sub_result(rhs);
        assert(ret.ok());
        return ret.unwrap();
    }
    [[nodiscard]] Decimal operator*(const Decimal& rhs) const noexcept {
        auto ret = mul_result(rhs);
        assert(ret.ok());
        return ret.unwrap();
    }
    [[nodiscard]] Decimal operator/(const Decimal& rhs) const noexcept {
        auto ret = div_result(rhs);
        assert(ret.ok());
        return ret.unwrap();
    }

    constexpr Decimal& operator+=(const Decimal& rhs) noexcept {
        *this = *this + rhs;
        return *this;
    }
    constexpr Decimal& operator-=(const Decimal& rhs) noexcept {
        *this = *this - rhs;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Decimal& rhs) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const Decimal& rhs) const noexcept {
        return raw_ < rhs.raw_;
    }
    [[nodiscard]] constexpr bool operator>(const Decimal& rhs) const noexcept {
        return rhs < *this;
    }
    [[nodiscard]] constexpr bool operator<=(const Decimal& rhs) const noexcept {
        return !(rhs < *this);
    }
    [[nodiscard]] constexpr bool operator>=(const Decimal& rhs) const noexcept {
        return !(*this < rhs);
    }

private:
    [[nodiscard]] static bool checked_add_(i64 a, i64 b, i64& out) noexcept {
        if((b > 0 && a > Limits<i64>::max() - b) || (b < 0 && a < Limits<i64>::min() - b)) {
            return false;
        }
        out = a + b;
        return true;
    }

    [[nodiscard]] static bool checked_sub_(i64 a, i64 b, i64& out) noexcept {
        if((b < 0 && a > Limits<i64>::max() + b) || (b > 0 && a < Limits<i64>::min() + b)) {
            return false;
        }
        out = a - b;
        return true;
    }

    [[nodiscard]] static bool checked_mul_(i64 a, i64 b, i64& out) noexcept {
        if(a == 0 || b == 0) {
            out = 0;
            return true;
        }

        if(a == -1) {
            if(b == Limits<i64>::min()) return false;
            out = -b;
            return true;
        }
        if(b == -1) {
            if(a == Limits<i64>::min()) return false;
            out = -a;
            return true;
        }

        if(a > 0) {
            if(b > 0) {
                if(a > Limits<i64>::max() / b) return false;
            } else {
                if(b < Limits<i64>::min() / a) return false;
            }
        } else {
            if(b > 0) {
                if(a < Limits<i64>::min() / b) return false;
            } else {
                if(a != 0 && b < Limits<i64>::max() / a) return false;
            }
        }

        out = a * b;
        return true;
    }

    i64 raw_ = 0;

    template<typename X>
    friend struct Reflect::Refl;
};

using Decimal_2 = Decimal<2>;
using Decimal_4 = Decimal<4>;
using Decimal_6 = Decimal<6>;
using Decimal_8 = Decimal<8>;

struct Deterministic_Duration {
    constexpr Deterministic_Duration() noexcept = default;
    explicit constexpr Deterministic_Duration(i64 nanoseconds) noexcept : ns_(nanoseconds) {
    }

    [[nodiscard]] static constexpr Deterministic_Duration from_ns(i64 ns) noexcept {
        return Deterministic_Duration{ns};
    }
    [[nodiscard]] static constexpr Deterministic_Duration from_us(i64 us) noexcept {
        return Deterministic_Duration{us * 1000};
    }
    [[nodiscard]] static constexpr Deterministic_Duration from_ms(i64 ms) noexcept {
        return Deterministic_Duration{ms * 1000000};
    }
    [[nodiscard]] static constexpr Deterministic_Duration from_s(i64 sec) noexcept {
        return Deterministic_Duration{sec * 1000000000};
    }

    [[nodiscard]] constexpr i64 ns() const noexcept {
        return ns_;
    }
    [[nodiscard]] constexpr i64 us() const noexcept {
        return ns_ / 1000;
    }
    [[nodiscard]] constexpr i64 ms() const noexcept {
        return ns_ / 1000000;
    }
    [[nodiscard]] constexpr i64 s() const noexcept {
        return ns_ / 1000000000;
    }

    [[nodiscard]] constexpr bool operator==(const Deterministic_Duration&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const Deterministic_Duration& rhs) const noexcept {
        return ns_ < rhs.ns_;
    }
    [[nodiscard]] constexpr Deterministic_Duration
    operator+(const Deterministic_Duration& rhs) const noexcept {
        return Deterministic_Duration{ns_ + rhs.ns_};
    }
    [[nodiscard]] constexpr Deterministic_Duration
    operator-(const Deterministic_Duration& rhs) const noexcept {
        return Deterministic_Duration{ns_ - rhs.ns_};
    }

private:
    i64 ns_ = 0;

    friend struct Reflect::Refl<Deterministic_Duration>;
};

struct Deterministic_Time {
    constexpr Deterministic_Time() noexcept = default;
    explicit constexpr Deterministic_Time(i64 unix_ns) noexcept : unix_ns_(unix_ns) {
    }

    [[nodiscard]] static constexpr Deterministic_Time from_unix_ns(i64 unix_ns) noexcept {
        return Deterministic_Time{unix_ns};
    }
    [[nodiscard]] static constexpr Deterministic_Time from_unix_us(i64 unix_us) noexcept {
        return Deterministic_Time{unix_us * 1000};
    }
    [[nodiscard]] static constexpr Deterministic_Time from_unix_ms(i64 unix_ms) noexcept {
        return Deterministic_Time{unix_ms * 1000000};
    }
    [[nodiscard]] static constexpr Deterministic_Time from_unix_s(i64 unix_s) noexcept {
        return Deterministic_Time{unix_s * 1000000000};
    }

    [[nodiscard]] constexpr i64 unix_ns() const noexcept {
        return unix_ns_;
    }
    [[nodiscard]] constexpr i64 unix_us() const noexcept {
        return unix_ns_ / 1000;
    }
    [[nodiscard]] constexpr i64 unix_ms() const noexcept {
        return unix_ns_ / 1000000;
    }
    [[nodiscard]] constexpr i64 unix_s() const noexcept {
        return unix_ns_ / 1000000000;
    }

    [[nodiscard]] constexpr Deterministic_Time
    operator+(Deterministic_Duration d) const noexcept {
        return Deterministic_Time{unix_ns_ + d.ns()};
    }
    [[nodiscard]] constexpr Deterministic_Time
    operator-(Deterministic_Duration d) const noexcept {
        return Deterministic_Time{unix_ns_ - d.ns()};
    }
    [[nodiscard]] constexpr Deterministic_Duration
    operator-(Deterministic_Time rhs) const noexcept {
        return Deterministic_Duration::from_ns(unix_ns_ - rhs.unix_ns_);
    }

    [[nodiscard]] constexpr bool operator==(const Deterministic_Time&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const Deterministic_Time& rhs) const noexcept {
        return unix_ns_ < rhs.unix_ns_;
    }

private:
    i64 unix_ns_ = 0;

    friend struct Reflect::Refl<Deterministic_Time>;
};

template<u32 Scale>
SPP_TEMPLATE_RECORD(Decimal, Scale, SPP_FIELD(raw_));
SPP_RECORD(Deterministic_Duration, SPP_FIELD(ns_));
SPP_RECORD(Deterministic_Time, SPP_FIELD(unix_ns_));

namespace Format {

template<u32 Scale>
struct Measure<Decimal<Scale>> {
    [[nodiscard]] static u64 measure(const Decimal<Scale>& value) noexcept {
        u64 length = 0;
        i64 raw = value.raw();
        if(raw < 0) length++;

        u64 mag = raw == Limits<i64>::min() ? (static_cast<u64>(Limits<i64>::max()) + 1)
                                            : static_cast<u64>(raw < 0 ? -raw : raw);
        u64 whole = mag / Decimal<Scale>::factor();

        u64 whole_digits = 1;
        while(whole >= 10) {
            whole /= 10;
            whole_digits++;
        }
        length += whole_digits;
        if constexpr(Scale > 0) {
            length += 1 + Scale;
        }
        return length;
    }
};

template<Allocator O, u32 Scale>
struct Write<O, Decimal<Scale>> {
    [[nodiscard]] static u64 write(String<O>& output, u64 idx, const Decimal<Scale>& value) noexcept {
        i64 raw = value.raw();
        if(raw < 0) {
            idx = output.write(idx, '-');
        }

        u64 mag = raw == Limits<i64>::min() ? (static_cast<u64>(Limits<i64>::max()) + 1)
                                            : static_cast<u64>(raw < 0 ? -raw : raw);
        u64 factor = Decimal<Scale>::factor();
        u64 whole = mag / factor;
        u64 frac = mag % factor;

#ifdef SPP_COMPILER_MSVC
        idx += Libc::snprintf(output.data() + idx, output.length() - idx, "%llu", whole);
#else
        idx += Libc::snprintf(output.data() + idx, output.length() - idx, "%lu", whole);
#endif
        if constexpr(Scale > 0) {
            idx = output.write(idx, '.');
            u64 div = factor / 10;
            for(u32 i = 0; i < Scale; i++) {
                u8 digit = static_cast<u8>(frac / div);
                idx = output.write(idx, static_cast<char>('0' + digit));
                frac %= div;
                if(div > 1) div /= 10;
            }
        }
        return idx;
    }
};

template<>
struct Measure<Deterministic_Duration> {
    [[nodiscard]] static u64 measure(const Deterministic_Duration& d) noexcept {
        return Measure<i64>::measure(d.ns()) + 2;
    }
};

template<Allocator O>
struct Write<O, Deterministic_Duration> {
    [[nodiscard]] static u64 write(String<O>& output, u64 idx,
                                   const Deterministic_Duration& d) noexcept {
        idx = Write<O, i64>::write(output, idx, d.ns());
        return output.write(idx, "ns"_v);
    }
};

template<>
struct Measure<Deterministic_Time> {
    [[nodiscard]] static u64 measure(const Deterministic_Time& t) noexcept {
        return Measure<i64>::measure(t.unix_ns()) + 3;
    }
};

template<Allocator O>
struct Write<O, Deterministic_Time> {
    [[nodiscard]] static u64 write(String<O>& output, u64 idx,
                                   const Deterministic_Time& t) noexcept {
        idx = Write<O, i64>::write(output, idx, t.unix_ns());
        return output.write(idx, "ns@"_v);
    }
};

} // namespace Format

} // namespace spp
