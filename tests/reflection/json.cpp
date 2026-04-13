#include "test.h"

enum class User_Kind {
    guest,
    admin,
};

struct User {
    i32 id;
    bool active;
    User_Kind kind;
};

SPP_ENUM(User_Kind, guest, SPP_CASE(guest), SPP_CASE(admin));
SPP_RECORD(User, SPP_FIELD(id), SPP_FIELD(active), SPP_FIELD(kind));

i32 main() {
    Test test{"empty"_v};

    {
        auto s = Json::stringify("hello \"json\""_v);
        assert(s == "\"hello \\\"json\\\"\""_v);
    }
    {
        User u{7, true, User_Kind::admin};
        auto s = Json::stringify(u);
        assert(s == "{\"id\":7,\"active\":true,\"kind\":\"admin\"}"_v);
    }
    {
        i32 a[3] = {1, 2, 3};
        auto s = Json::stringify(a);
        assert(s == "[1,2,3]"_v);
    }
    {
        Vec<i32> v{1, 2, 3};
        auto s = Json::stringify(v);
        assert(s == "[1,2,3]"_v);
    }
    {
        Map<String_View, i32> m;
        m.insert("x"_v, 1);
        m.insert("y"_v, 2);
        auto s = Json::stringify(m);
        assert(s == "{\"x\":1,\"y\":2}"_v || s == "{\"y\":2,\"x\":1}"_v);
    }
    {
        User u{7, true, User_Kind::admin};
        auto pretty = Json::stringify_pretty(u, 2);
        assert(pretty == "{\n  \"id\": 7,\n  \"active\": true,\n  \"kind\": \"admin\"\n}"_v);
    }
    {
        auto b = Json::parse_result<bool>(" true "_v);
        assert(b.ok() && b.unwrap() == true);
        auto i = Json::parse_result<i64>("42"_v);
        assert(i.ok() && i.unwrap() == 42);
        auto s = Json::parse_result<String<Mdefault>>("\"hello\""_v);
        assert(s.ok() && s.unwrap() == "hello"_v);
    }
    {
        auto v = Json::parse_vec_result<i32>("[1, 2, 3]"_v);
        assert(v.ok());
        assert(v.unwrap().length() == 3);
        assert(v.unwrap()[0] == 1 && v.unwrap()[1] == 2 && v.unwrap()[2] == 3);
    }
    {
        Opt<i32> none;
        auto s0 = Json::stringify(none);
        assert(s0 == "null"_v);

        Opt<i32> some{7};
        auto s1 = Json::stringify(some);
        assert(s1 == "7"_v);
    }
    {
        auto ok = Result<i32, String_View>::ok(11);
        auto e = Result<i32, String_View>::err("bad"_v);
        auto so = Json::stringify(ok);
        auto se = Json::stringify(e);
        assert(so == "{\"ok\":11}"_v);
        assert(se == "{\"err\":\"bad\"}"_v);
    }

    return 0;
}
