
#include "test.h"

namespace spp {

struct Counting_Key {
    static inline i32 live = 0;
    i32 value = 0;

    Counting_Key() noexcept {
        live++;
    }
    explicit Counting_Key(i32 v) noexcept : value(v) {
        live++;
    }
    Counting_Key(const Counting_Key& src) noexcept : value(src.value) {
        live++;
    }
    Counting_Key(Counting_Key&& src) noexcept : value(src.value) {
        live++;
    }
    ~Counting_Key() noexcept {
        live--;
    }

    [[nodiscard]] bool operator==(const Counting_Key& other) const noexcept {
        return value == other.value;
    }
};

struct Counting_Value {
    static inline i32 live = 0;
    i32 value = 0;

    Counting_Value() noexcept {
        live++;
    }
    explicit Counting_Value(i32 v) noexcept : value(v) {
        live++;
    }
    Counting_Value(const Counting_Value& src) noexcept : value(src.value) {
        live++;
    }
    Counting_Value(Counting_Value&& src) noexcept : value(src.value) {
        live++;
    }
    ~Counting_Value() noexcept {
        live--;
    }
};

namespace Hash {
template<>
struct Hash<Counting_Key> {
    [[nodiscard]] static u64 hash(const Counting_Key& key) noexcept {
        return squirrel5(static_cast<u64>(key.value));
    }
};
} // namespace Hash

} // namespace spp

i32 main() {
    Test test{"map"_v};
    Trace("Map") {
        auto deduct = Map{Pair{"foo"_v, 0}, Pair{"bar"_v, 1}};
        static_assert(Same<decltype(deduct), Map<String_View, i32>>);

        {
            Map<String_View, i32> int_map{Pair{"foo"_v, 0}, Pair{"bar"_v, 1}};

            int_map.insert("baz"_v, 2);
            int_map.erase("bar"_v);

            for(auto& [key, value] : int_map) info("%: %", key, value);
        }

        {
            Map<String<>, String<>> sv{Pair{"Hello"_v.string(), "World"_v.string()}};
            //
        }

        Map<i32, i32> v;
        v.insert(1, 1);
        v.insert(2, 2);
        v.insert(3, 3);

        for(auto [k, vv] : v) {
            vv = k;
            info("% %", k, vv);
        }
        for(auto& [k, vv] : v) {
            vv = k;
            info("% %", k, vv);
        }

        const auto& constv = v;
        for(auto [k, vv] : constv) {
            k = 0;
            vv = k;
            info("% %", k, vv);
        }
        for(const auto& [k, vv] : constv) {
            info("% %", k, vv);
        }

        assert(v.length() == 3);
        assert(v.get(1) == 1);

        v.erase(2);
        assert(v.length() == 2);
        assert(v.get(3) == 3);

        Map<i32, i32> v2 = v.clone();
        Map<i32, i32> v3 = move(v2);

        assert(v3.length() == 2);
        Map<i32, i32, Mhidden> v4 = v.clone<Mhidden>();
        assert(v4.length() == 2);
        assert(v4.get(1) == 1);
        assert(v4.get(3) == 3);

        Map<i32, String_View> i_sv{Pair{1, "Hello"_v}, Pair{2, "World"_v}};
        Map<String_View, i32> sv_i{Pair{"Hello"_v, 1}, Pair{"World"_v, 2}};

        Map<String_View, String_View> sv{Pair{"Hello"_v, "World"_v}};
        const auto& csv = sv;
        assert(csv.contains("Hello"_v));
        assert(csv.get("Hello"_v) == "World"_v);
        auto maybe = csv.try_get("Hello"_v);
        assert(maybe.ok());
        assert(**maybe == "World"_v);
        auto missing = csv.try_get("Missing"_v);
        assert(!missing.ok());

        Map<String_View, String_View> sv2 = sv.clone();
        Map<String_View, String_View> sv3 = move(sv2);

        assert(sv3.length() == 1);

        Map<i32, Function<void()>> ff;
        for(i32 i = 0; i < 40; i++) {
            ff.insert(i, []() { info("Hello"); });
        }

        {
            assert(Counting_Key::live == 0);
            assert(Counting_Value::live == 0);
            Map<Counting_Key, Counting_Value> lifecycle;
            for(i32 i = 0; i < 128; i++) {
                lifecycle.insert(Counting_Key{i}, Counting_Value{i * 2});
            }
            assert(lifecycle.length() == 128);
            for(i32 i = 0; i < 64; i++) {
                assert(lifecycle.try_erase(Counting_Key{i}));
            }
            assert(lifecycle.length() == 64);
        }
        assert(Counting_Key::live == 0);
        assert(Counting_Value::live == 0);
    }
    return 0;
}
