#include "test.h"

i32 main() {
    Test test{"empty"_v};

    Trace("Result") {
        Result<i32, String_View> ok = Result<i32, String_View>::ok(7);
        assert(ok.ok());
        assert(ok.unwrap() == 7);

        Result<i32, String_View> err = Result<i32, String_View>::err("bad"_v);
        assert(!err.ok());
        assert(err.unwrap_err() == "bad"_v);

        auto ok2 = ok.clone();
        auto err2 = err.clone();
        assert(ok2.ok());
        assert(ok2.unwrap() == 7);
        assert(!err2.ok());
        assert(err2.unwrap_err() == "bad"_v);

        ok2.emplace_err("oops"_v);
        assert(!ok2.ok());
        assert(ok2.unwrap_err() == "oops"_v);

        err2.emplace_ok(42);
        assert(err2.ok());
        assert(err2.unwrap() == 42);

        auto mapped = Result<i32, String_View>::ok(3).map([](i32 v) { return v + 4; });
        assert(mapped.ok());
        assert(mapped.unwrap() == 7);

        auto mapped_err =
            Result<i32, String_View>::err("boom"_v).map_err([](String_View e) -> i32 {
                return static_cast<i32>(e.length());
            });
        assert(!mapped_err.ok());
        assert(mapped_err.unwrap_err() == 4);

        auto chained = Result<i32, String_View>::ok(5).and_then([](i32 v) {
            if(v > 0) return Result<i32, String_View>::ok(v * 2);
            return Result<i32, String_View>::err("neg"_v);
        });
        assert(chained.ok());
        assert(chained.unwrap() == 10);

        auto recovered = Result<i32, String_View>::err("bad"_v).or_else([](String_View) {
            return Result<i32, String_View>::ok(9);
        });
        assert(recovered.ok());
        assert(recovered.unwrap() == 9);
    }

    return 0;
}
