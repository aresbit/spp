#include "test.h"

i32 main() {
    Test test{"empty"_v};

    Trace("Decimal deterministic arithmetic") {
        using D2 = Decimal<2>;
        D2 a = D2::from_raw(12345);  // 123.45
        D2 b = D2::from_raw(55);     // 0.55

        auto add = a.add_result(b);
        assert(add.ok());
        assert(add.unwrap().raw() == 12400);

        auto sub = a.sub_result(b);
        assert(sub.ok());
        assert(sub.unwrap().raw() == 12290);

        auto mul = a.mul_result(D2::from_raw(200)); // 2.00
        assert(mul.ok());
        assert(mul.unwrap().raw() == 24690);        // 246.90

        auto div = a.div_result(D2::from_raw(300)); // /3.00
        assert(div.ok());
        assert(div.unwrap().raw() == 4115);         // 41.15

        auto ratio = D2::from_ratio_result(7, 2);
        assert(ratio.ok());
        assert(ratio.unwrap().raw() == 350);        // 3.50

        auto fmt = format<Mdefault>("% % %"_v, D2::from_raw(-1205), D2::from_raw(0), D2::from_raw(3));
        assert(fmt == "-12.05 0.00 0.03"_v);

        // Compound assignment and helper accessors.
        D2 acc = D2::from_raw(1000);
        acc *= D2::from_raw(200);  // 10.00 * 2.00 = 20.00
        assert(acc.raw() == 2000);
        acc /= D2::from_raw(400);  // 20.00 / 4.00 = 5.00
        assert(acc.raw() == 500);

        assert(D2::from_raw(-1).is_negative());
        assert(!D2::from_raw(0).is_negative());
        assert(D2::from_raw(-7).abs_result().unwrap().raw() == 7);
        assert(D2::from_raw(7).abs_result().unwrap().raw() == 7);
        assert(D2::from_raw(7).negate_result().unwrap().raw() == -7);

        auto int_ok = D2::from_int_result(42);
        assert(int_ok.ok());
        assert(int_ok.unwrap().raw() == 4200);
        auto int_overflow = D2::from_int_result(Limits<i64>::max() / 50);
        assert(!int_overflow.ok());
    }

    Trace("Deterministic time and duration") {
        Deterministic_Duration d0 = Deterministic_Duration::from_ms(1500);
        assert(d0.ns() == 1500000000);
        assert(d0.ms() == 1500);
        assert(d0.s() == 1);

        Deterministic_Time t0 = Deterministic_Time::from_unix_s(1700000000);
        Deterministic_Time t1 = t0 + d0;
        assert(t1.unix_ns() == t0.unix_ns() + d0.ns());
        assert((t1 - t0).ns() == d0.ns());

        auto printed = format<Mdefault>("% %"_v, d0, t1);
        assert(printed.length() > 0);

        // Overflow-checked duration arithmetic.
        auto add_ok = d0.add_result(Deterministic_Duration::from_ms(500));
        assert(add_ok.ok());
        assert(add_ok.unwrap().ms() == 2000);

        Deterministic_Duration big = Deterministic_Duration::from_ns(Limits<i64>::max());
        auto add_overflow = big.add_result(Deterministic_Duration::from_ns(1));
        assert(!add_overflow.ok());

        Deterministic_Duration tiny = Deterministic_Duration::from_ns(Limits<i64>::min());
        auto sub_overflow = tiny.sub_result(Deterministic_Duration::from_ns(1));
        assert(!sub_overflow.ok());

        assert(d0 >= Deterministic_Duration::from_ms(1500));
        assert(d0 > Deterministic_Duration::from_ms(1000));
        assert(d0 <= Deterministic_Duration::from_ms(2000));
    }

    return 0;
}
