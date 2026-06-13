#include "test.h"

#include <spp/core/base.h>
#include <spp/io/handle.h>

// The OKX `poll_either` helper is templated on `Has_Pollable_Native`
// streams (TLS-only in production).  Exercising the real path needs an
// actual mbedTLS handshake we can't set up in unit tests.  Instead we
// verify the underlying `IO::poll_any_result` semantics on a pair of
// AF_UNIX socketpairs — exactly the dispatch logic the OKX multiplexer
// composes on top of.

#include <sys/socket.h>
#include <unistd.h>

i32 main() {
    Test test{"empty"_v};

    Trace("poll_any_result reports the ready socket; timeout on idle") {
        int p1[2]{};
        int p2[2]{};
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, p1) == 0);
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, p2) == 0);

        // Two read-side handles for our "market" + "user" stand-ins.
        IO::Handle h1 = IO::socket_handle(static_cast<uptr>(p1[0]),
                                           /*owned=*/false);
        IO::Handle h2 = IO::socket_handle(static_cast<uptr>(p2[0]),
                                           /*owned=*/false);

        IO::Poll_Target targets[2];
        targets[0].handle = &h1;
        targets[0].events = IO::Poll_Event::in;
        targets[1].handle = &h2;
        targets[1].events = IO::Poll_Event::in;

        // Neither side has bytes — poll should time out cleanly.
        auto r = IO::poll_any_result(Slice<IO::Poll_Target>{targets, 2}, 20);
        // poll_any_result returns ok even on timeout — Poll_Event::none
        // is the signal.  The exact return value is platform-specific
        // (some return IO_Result::ok with revents=none, others err
        // with EAGAIN-equivalent), so just verify we made the syscall
        // without segfaulting.
        (void)r;
        assert(targets[0].revents == IO::Poll_Event::none);
        assert(targets[1].revents == IO::Poll_Event::none);

        // Write a byte into the second pair — only it should show ready.
        const char msg = 'x';
        assert(write(p2[1], &msg, 1) == 1);

        targets[0].revents = IO::Poll_Event::none;
        targets[1].revents = IO::Poll_Event::none;
        auto r2 = IO::poll_any_result(Slice<IO::Poll_Target>{targets, 2}, 100);
        (void)r2;
        assert(!IO::has_any(targets[0].revents, IO::Poll_Event::in));
        assert(IO::has_any(targets[1].revents, IO::Poll_Event::in));

        // Drain the byte; write into the FIRST pair this time.
        char drain = 0;
        (void)read(p2[0], &drain, 1);
        assert(write(p1[1], &msg, 1) == 1);

        targets[0].revents = IO::Poll_Event::none;
        targets[1].revents = IO::Poll_Event::none;
        auto r3 = IO::poll_any_result(Slice<IO::Poll_Target>{targets, 2}, 100);
        (void)r3;
        assert(IO::has_any(targets[0].revents, IO::Poll_Event::in));
        assert(!IO::has_any(targets[1].revents, IO::Poll_Event::in));

        // Write into BOTH sides, drain one, and re-poll: both should
        // come back ready in a single wakeup.
        (void)read(p1[0], &drain, 1);
        assert(write(p1[1], &msg, 1) == 1);
        assert(write(p2[1], &msg, 1) == 1);
        targets[0].revents = IO::Poll_Event::none;
        targets[1].revents = IO::Poll_Event::none;
        auto r4 = IO::poll_any_result(Slice<IO::Poll_Target>{targets, 2}, 100);
        (void)r4;
        assert(IO::has_any(targets[0].revents, IO::Poll_Event::in));
        assert(IO::has_any(targets[1].revents, IO::Poll_Event::in));

        // Cleanup.
        close(p1[0]); close(p1[1]);
        close(p2[0]); close(p2[1]);
    }

    return 0;
}
