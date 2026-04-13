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

    return 0;
}
