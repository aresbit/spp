#include "test.h"

#include <spp/io/files.h>

i32 main() {
    Test test{"empty"_v};

    String_View path = "tmp_mmap.bin"_v;
    {
        auto ex = Files::exists_result(path);
        assert(ex.ok());
        if(ex.unwrap()) {
            auto rm = Files::remove_result(path);
            assert(rm.ok());
        }
    }

    {
        Array<u8, 4096> init{};
        init[0] = u8{'a'};
        init[1] = u8{'b'};
        init[4095] = u8{'z'};
        auto w = Files::write_result(path, init.slice());
        assert(w.ok());
        assert(w.unwrap() == 4096);
    }

    {
        auto m = Files::mmap_result(path, 4096);
        assert(m.ok());
        auto mapped = m.unwrap();
        assert(mapped.length == 4096);
        mapped.data[0] = u8{'Q'};
        mapped.data[4095] = u8{'W'};

        auto s = Files::msync_result(mapped);
        assert(s.ok());

        auto u = Files::munmap_result(mapped);
        assert(u.ok());
    }

    {
        auto all = Files::read_result(path);
        assert(all.ok());
        assert(all.unwrap().length() == 4096);
        assert(all.unwrap()[0] == 'Q');
        assert(all.unwrap()[4095] == 'W');
    }

    {
        auto m = Files::mmap_result(path, 0);
        assert(m.ok());
        auto mapped = m.unwrap();
        assert(mapped.length == 4096);
        mapped.data[1] = u8{'R'};
        auto s = Files::msync_result(mapped);
        assert(s.ok());
        auto u = Files::munmap_result(mapped);
        assert(u.ok());
    }

    {
        auto all = Files::read_result(path);
        assert(all.ok());
        assert(all.unwrap()[1] == 'R');
    }

    {
        auto rm = Files::remove_result(path);
        assert(rm.ok());
    }

    return 0;
}
