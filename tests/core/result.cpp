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
    }

    return 0;
}
