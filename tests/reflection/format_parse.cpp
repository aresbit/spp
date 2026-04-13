#include "test.h"

enum class Parse_Enum {
    alpha,
    beta,
};

SPP_ENUM(Parse_Enum, alpha, SPP_CASE(alpha), SPP_CASE(beta));

i32 main() {
    Test test{"empty"_v};

    using namespace Format;

    {
        auto r = parse_i64_result("123 rest"_v);
        assert(r.ok());
        assert(r.unwrap().first == 123);
        assert(r.unwrap().second == " rest"_v);
    }
    {
        auto r = parse_i64_result("abc"_v);
        assert(!r.ok());
    }

    {
        auto r = parse_f32_result("1.5 z"_v);
        assert(r.ok());
        assert(r.unwrap().first > 1.49f && r.unwrap().first < 1.51f);
        assert(r.unwrap().second == " z"_v);
    }
    {
        auto r = parse_f32_result("x"_v);
        assert(!r.ok());
    }

    {
        auto r = parse_string_result("  hello world"_v);
        assert(r.ok());
        assert(r.unwrap().first == "hello"_v);
        assert(r.unwrap().second == "world"_v);
    }
    {
        auto r = parse_string_result("   "_v);
        assert(!r.ok());
    }

    {
        auto r = parse_enum_result<Parse_Enum>("alpha rest"_v);
        assert(r.ok());
        assert(r.unwrap().first == Parse_Enum::alpha);
        assert(r.unwrap().second == "rest"_v);
    }
    {
        auto r = parse_enum_result<Parse_Enum>("gamma"_v);
        assert(!r.ok());
    }

    return 0;
}
