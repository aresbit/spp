#include "test.h"

#include <spp/io/handle.h>

i32 main() {
    Test test{"handle"_v};

    auto caps = IO::capability_matrix();
    assert(caps.file_handles);
    assert(caps.socket_handles);
    assert(caps.pipe_handles);
    assert(caps.poll_pipe);

    auto p = IO::pipe_result();
    assert(p.ok());
    auto pipe = spp::move(p.unwrap());

    Array<IO::Poll_Target, 1> targets{
        IO::Poll_Target{&pipe.read, IO::Poll_Event::in, IO::Poll_Event::none}};

    auto ready0 = IO::poll_any_result(targets.slice(), 1);
    assert(!ready0.ok());
    assert(ready0.unwrap_err() == "timeout"_v);

    Array<u8, 4> out{u8{'P'}, u8{'I'}, u8{'P'}, u8{'E'}};
    auto wrote = IO::write_some_result(pipe.write, out.slice());
    assert(wrote.ok());
    assert(wrote.unwrap() == 4);

    auto ready1 = IO::poll_any_result(targets.slice(), 50);
    assert(ready1.ok());
    assert(ready1.unwrap().ready_index == 0);
    assert(IO::has_any(ready1.unwrap().events, IO::Poll_Event::in));

    Array<u8, 4> in{};
    auto read = IO::read_some_result(pipe.read, in.slice());
    assert(read.ok());
    assert(read.unwrap() == 4);
    for(u64 i = 0; i < in.length(); i++) {
        assert(in[i] == out[i]);
    }

    assert(IO::close_result(pipe.read).ok());
    assert(IO::close_result(pipe.write).ok());
    return 0;
}
