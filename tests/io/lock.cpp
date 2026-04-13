#include "test.h"

#include <spp/io/files.h>

i32 main() {
    Test test{"empty"_v};

    String_View lock_path = "tmp_db.lock"_v;

    {
        auto rel = Files::release_lock_result(lock_path);
        assert(rel.ok());
    }
    {
        auto acq = Files::acquire_lock_result(lock_path);
        assert(acq.ok());
    }
    {
        auto acq2 = Files::acquire_lock_result(lock_path);
        assert(!acq2.ok());
    }
    {
        auto rel = Files::release_lock_result(lock_path);
        assert(rel.ok());
    }
    {
        auto acq3 = Files::acquire_lock_result(lock_path);
        assert(acq3.ok());
    }
    {
        auto rel = Files::release_lock_result(lock_path);
        assert(rel.ok());
    }

    return 0;
}
