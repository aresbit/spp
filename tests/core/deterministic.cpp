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
    }

    return 0;
}
