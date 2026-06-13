#include "test.h"

#include <spp/io/net.h>
#include <spp/io/stream.h>

// Concept conformance proof. If either of these starts failing it means the
// public API of the corresponding type drifted away from the contract every
// upper-layer protocol (Http::fetch, Protocol::Websocket::send_frame, future
// TLS adapters) depends on.
static_assert(Net::Byte_Stream<Net::Tcp_Client>,
              "Tcp_Client must satisfy Byte_Stream");
static_assert(Net::Byte_Stream<Net::Memory_Stream>,
              "Memory_Stream must satisfy Byte_Stream");

i32 main() {
    Test test{"empty"_v};

    Trace("Memory_Stream round-trip") {
        Net::Memory_Stream m;

        // Producer side: bytes written via send_all_result land in sent().
        u8 out_bytes[] = {'h', 'e', 'l', 'l', 'o'};
        auto sent = m.send_all_result(Slice<const u8>{out_bytes, 5});
        assert(sent.ok());
        assert(sent.unwrap() == 5);
        auto seen = m.sent();
        assert(seen.length() == 5);
        for(u64 i = 0; i < 5; i++) assert(seen[i] == out_bytes[i]);

        // Consumer side: inject() preloads bytes the consumer will see.
        m.inject("ABCDE"_v);
        u8 in_bytes[3];
        auto got = m.recv_result(Slice<u8>{in_bytes, 3});
        assert(got.ok());
        assert(got.unwrap() == 3);
        assert(in_bytes[0] == 'A' && in_bytes[1] == 'B' && in_bytes[2] == 'C');

        u8 rest[2];
        auto exact = m.recv_exact_result(Slice<u8>{rest, 2});
        assert(exact.ok());
        assert(rest[0] == 'D' && rest[1] == 'E');

        // Exhausted: recv reports 0 (EOF semantics), recv_exact errors out.
        u8 nothing[1];
        auto eof = m.recv_result(Slice<u8>{nothing, 1});
        assert(eof.ok());
        assert(eof.unwrap() == 0);

        auto short_read = m.recv_exact_result(Slice<u8>{nothing, 1});
        assert(!short_read.ok());
        assert(short_read.unwrap_err() == "short_read"_v);

        m.close();
        assert(m.closed());
        auto blocked = m.send_all_result(Slice<const u8>{out_bytes, 1});
        assert(!blocked.ok());
        assert(blocked.unwrap_err() == "closed"_v);
    }

    Trace("Stream_Adapter dispatches through the virtual interface") {
        Net::Memory_Stream backing;
        backing.inject("ZZZZ"_v);

        // Stream_Adapter is type-erased; callers that need runtime polymorphism
        // (e.g. plain-TCP vs TLS chosen at config time) hold a Stream& only.
        Net::Stream_Adapter<Net::Memory_Stream> adapter{backing};
        Net::Stream& generic = adapter;

        u8 buf[4];
        auto got = generic.recv_exact_result(Slice<u8>{buf, 4});
        assert(got.ok());
        for(u64 i = 0; i < 4; i++) assert(buf[i] == 'Z');

        u8 ping[3] = {'p', 'i', 'n'};
        auto sent = generic.send_all_result(Slice<const u8>{ping, 3});
        assert(sent.ok());
        auto observed = backing.sent();
        assert(observed.length() == 3);
        assert(observed[0] == 'p' && observed[1] == 'i' && observed[2] == 'n');

        generic.close();
        assert(backing.closed());
    }

    return 0;
}
