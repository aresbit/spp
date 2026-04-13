
#include "test.h"

#include <spp/core/rc.h>

i32 main() {
    Profile::begin_frame();
    {
        Test test{"empty"_v};
        Region(R0) {
            auto v0 = Vec<u8, Mregion<R0>>::make(Math::MB(2));
            Region(R1) {
                auto v1 = Vec<u8, Mregion<R1>>::make(Math::MB(4));
                Region(R2) {
                    auto v2 = Vec<u8, Mregion<R2>>::make(256);
                    auto v3 = Vec<u8, Mregion<R2>>::make(256);
                    auto v4 = Vec<u8, Mregion<R2>>::make(256);
                    Region(R3) {
                        auto v5 = Vec<u8, Mregion<R3>>::make(256);
                        auto v6 = Vec<u8, Mregion<R3>>::make(256);
                        auto v7 = Vec<u8, Mregion<R3>>::make(Math::MB(2));
                    }
                }
            }
        }
        Trace("Alloc0") {
            using A = Mallocator<"Test">;
            void* ptr = A::alloc(100);
            A::free(ptr);

            {
                Vec<i32, Mhidden> hidden_vec;
                hidden_vec.push(1);
                hidden_vec.push(2);
                hidden_vec.push(3);
                assert(hidden_vec.length() == 3);

                Vec<i32, Mdefault> default_vec = hidden_vec.clone<Mdefault>();
                assert(default_vec.length() == hidden_vec.length());
                for(u64 i = 0; i < default_vec.length(); i++) {
                    assert(default_vec[i] == hidden_vec[i]);
                }
            }
            {
                Box<i32, Mhidden> hidden_box{42};
                Box<i32, Mdefault> default_box = hidden_box.clone<Mdefault>();
                assert(default_box.ok());
                assert(*default_box == 42);
            }
            {
                Rc<i32, Mhidden> hidden_rc{7};
                Rc<i32, Mdefault> default_rc = hidden_rc.clone<Mdefault>();
                assert(hidden_rc.ok());
                assert(default_rc.ok());
                assert(*hidden_rc == 7);
                assert(*default_rc == 7);
                assert(hidden_rc.references() == 1);
                assert(default_rc.references() == 1);
            }
            {
                Arc<i32, Mhidden> hidden_arc{9};
                Arc<i32, Mdefault> default_arc = hidden_arc.clone<Mdefault>();
                assert(hidden_arc.ok());
                assert(default_arc.ok());
                assert(*hidden_arc == 9);
                assert(*default_arc == 9);
                assert(hidden_arc.references() == 1);
                assert(default_arc.references() == 1);
            }

            {
                for(u64 i = 0; i < 100; i++) {
                    i32* n = Mpool::make<i32>();
                    Mpool::destroy(n);
                }

                Box<i32, Mpool> pool_box1{1};
                Box<i32, Mpool> pool_box2{2};
                Rc<i32, Mpool> pool_rc1{1};
                Rc<i32, Mpool> pool_rc2{2};
                Arc<i32, Mpool> pool_arc1{1};
                Arc<i32, Mpool> pool_arc2{2};
            }
            {
                for(u64 i = 0; i < 100; i++) {
                    auto* n = Mpool::make<Box<i32, A>>();
                    Mpool::destroy(n);
                }

                Box<Box<i32, Mpool>, Mpool> pool_box1{1};
                Box<Box<i32, Mpool>, Mpool> pool_box2{2};
                Rc<Box<i32, Mpool>, Mpool> pool_rc1{1};
                Rc<Box<i32, Mpool>, Mpool> pool_rc2{2};
                Arc<Box<i32, Mpool>, Mpool> pool_arc1{1};
                Arc<Box<i32, Mpool>, Mpool> pool_arc2{2};
            }
        }
    }
    Profile::end_frame();
    Profile::finalize();
    return 0;
}
