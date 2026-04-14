#include "test.h"

#include <spp/io/wal.h>

i32 main() {
    Test test{"empty"_v};

    String_View path = "tmp_wal.log"_v;
    {
        auto ex = Files::exists_result(path);
        assert(ex.ok());
        if(ex.unwrap()) {
            auto rm = Files::remove_result(path);
            assert(rm.ok());
        }
    }

    WAL::Writer writer;
    {
        auto open = writer.open_result(path, 4096, 8);
        assert(open.ok());
    }

    {
        Array<u8, 4> a{u8{'R'}, u8{'E'}, u8{'C'}, u8{'1'}};
        auto w = writer.append_result(a.slice());
        assert(w.ok() && w.unwrap() == 4);
    }

    {
        Array<u8, 2> b{u8{'A'}, u8{'B'}};
        Array<u8, 2> c{u8{'C'}, u8{'D'}};
        Array<Files::Write_IO_Slice, 2> ws{
            Files::Write_IO_Slice{b.data(), b.length()},
            Files::Write_IO_Slice{c.data(), c.length()},
        };
        auto w = writer.appendv_result(ws.slice());
        assert(w.ok() && w.unwrap() == 4);
    }

    {
        Array<u8, 4> d{u8{'R'}, u8{'E'}, u8{'C'}, u8{'2'}};
        auto w = writer.append_result(d.slice());
        assert(w.ok() && w.unwrap() == 4);
    }

    {
        auto f = writer.flush_result();
        assert(f.ok());
    }

    {
        auto all = Files::read_result(path);
        assert(all.ok());
        assert(all.unwrap().length() == 4096);
        assert(all.unwrap()[0] == 'R');
        assert(all.unwrap()[1] == 'E');
        assert(all.unwrap()[2] == 'C');
        assert(all.unwrap()[3] == '1');
        assert(all.unwrap()[4] == 'A');
        assert(all.unwrap()[5] == 'B');
        assert(all.unwrap()[6] == 'C');
        assert(all.unwrap()[7] == 'D');
        assert(all.unwrap()[8] == 'R');
        assert(all.unwrap()[9] == 'E');
        assert(all.unwrap()[10] == 'C');
        assert(all.unwrap()[11] == '2');
    }

    {
        auto m = Files::mmap_result(path, 0);
        assert(m.ok());
        auto mapped = m.unwrap();

        auto p1 = WAL::apply_page_cache_policy_result(mapped, WAL::Page_Cache_Policy::append_sequential);
        auto p2 = WAL::apply_page_cache_policy_result(mapped, WAL::Page_Cache_Policy::drop_soon);
        assert(p1.ok());
        assert(p2.ok());

        auto u = Files::munmap_result(mapped);
        assert(u.ok());
    }

    {
        auto rm = Files::remove_result(path);
        assert(rm.ok());
    }

    return 0;
}
