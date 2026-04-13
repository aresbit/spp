#include "test.h"

#include <spp/io/files.h>

i32 main() {
    Test test{"empty"_v};

    String_View path = "tmp_files_io.bin"_v;
    {
        Array<u8, 8> init{u8{'a'}, u8{'b'}, u8{'c'}, u8{'d'}, u8{'e'}, u8{'f'}, u8{'g'}, u8{'h'}};
        auto w = Files::write_result(path, init.slice());
        assert(w.ok() && w.unwrap() == 8);
    }
    {
        Array<u8, 3> patch{u8{'X'}, u8{'Y'}, u8{'Z'}};
        auto w = Files::pwrite_result(path, 2, patch.slice());
        assert(w.ok() && w.unwrap() == 3);
    }
    {
        Array<u8, 8> out{};
        auto r = Files::pread_result(path, 0, out.slice());
        assert(r.ok() && r.unwrap() == 8);
        assert(out[0] == 'a');
        assert(out[1] == 'b');
        assert(out[2] == 'X');
        assert(out[3] == 'Y');
        assert(out[4] == 'Z');
        assert(out[5] == 'f');
        assert(out[6] == 'g');
        assert(out[7] == 'h');
    }
    {
        auto all = Files::read_result(path);
        assert(all.ok());
        assert(all.unwrap().length() == 8);
    }

    return 0;
}
