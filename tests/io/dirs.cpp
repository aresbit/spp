#include "test.h"

#include <spp/io/files.h>

i32 main() {
    Test test{"empty"_v};

    String_View dir = "tmp_files_dir"_v;
    {
        auto e = Files::exists_result(dir);
        assert(e.ok());
        if(e.unwrap()) {
            auto rd = Files::rmdir_result(dir);
            assert(rd.ok());
        }
    }
    {
        auto mk = Files::mkdir_result(dir);
        assert(mk.ok());
    }
    {
        auto e = Files::exists_result(dir);
        assert(e.ok() && e.unwrap());
    }
    {
        auto rd = Files::rmdir_result(dir);
        assert(rd.ok());
    }
    {
        auto e = Files::exists_result(dir);
        assert(e.ok() && !e.unwrap());
    }

    return 0;
}
