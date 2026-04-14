#include "test.h"

#include <spp/io/files.h>

i32 main() {
    Test test{"empty"_v};

    String_View path = "tmp_iov.bin"_v;
    {
        auto ex = Files::exists_result(path);
        assert(ex.ok());
        if(ex.unwrap()) {
            auto rm = Files::remove_result(path);
            assert(rm.ok());
        }
    }

    {
        Array<u8, 32> init{};
        auto w = Files::write_result(path, init.slice());
        assert(w.ok() && w.unwrap() == 32);
    }

    Array<u8, 2> a{u8{'A'}, u8{'B'}};
    Array<u8, 2> b{u8{'C'}, u8{'D'}};
    Array<u8, 2> c{u8{'E'}, u8{'F'}};
    Array<Files::Write_IO_Slice, 3> ws{
        Files::Write_IO_Slice{a.data(), a.length()},
        Files::Write_IO_Slice{b.data(), b.length()},
        Files::Write_IO_Slice{c.data(), c.length()},
    };
    {
        auto wv = Files::pwritev_result(path, 4, ws.slice());
        assert(wv.ok() && wv.unwrap() == 6);
    }
    {
        auto fd = Files::fdatasync_result(path);
        assert(fd.ok());
    }

    Array<u8, 2> ra{};
    Array<u8, 2> rb{};
    Array<u8, 2> rc{};
    Array<Files::Read_IO_Slice, 3> rs{
        Files::Read_IO_Slice{ra.data(), ra.length()},
        Files::Read_IO_Slice{rb.data(), rb.length()},
        Files::Read_IO_Slice{rc.data(), rc.length()},
    };
    {
        auto rv = Files::preadv_result(path, 4, rs.slice());
        assert(rv.ok() && rv.unwrap() == 6);
        assert(ra[0] == 'A' && ra[1] == 'B');
        assert(rb[0] == 'C' && rb[1] == 'D');
        assert(rc[0] == 'E' && rc[1] == 'F');
    }

    {
        auto fa = Files::fallocate_result(path, 8192);
        assert(fa.ok() && fa.unwrap() == 8192);
    }
    {
        auto mm = Files::mmap_result(path, 0);
        assert(mm.ok());
        auto mapped = mm.unwrap();
        assert(mapped.length >= 8192);
        auto md = Files::madvise_result(mapped, Files::Madvise_Hint::sequential);
        assert(md.ok());
        auto um = Files::munmap_result(mapped);
        assert(um.ok());
    }

    {
        auto all = Files::read_result(path);
        assert(all.ok());
        assert(all.unwrap().length() == 8192);
        assert(all.unwrap()[4] == 'A');
        assert(all.unwrap()[5] == 'B');
        assert(all.unwrap()[6] == 'C');
        assert(all.unwrap()[7] == 'D');
        assert(all.unwrap()[8] == 'E');
        assert(all.unwrap()[9] == 'F');
    }

    {
        auto rm = Files::remove_result(path);
        assert(rm.ok());
    }

    return 0;
}
