#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/websocket.h>

namespace Ws = Protocol::Websocket;

i32 main() {
    Test test{"empty"_v};

    Trace("Websocket short text frame round-trip") {
        u8 payload[] = {'h', 'i'};
        Ws::Frame f;
        f.fin = true;
        f.op = Ws::Opcode::text;
        f.payload = Slice<const u8>{payload, 2};

        Vec<u8, Mdefault> wire;
        Ws::encode(wire, f, false);
        // Server-to-client unmasked: 0x81 0x02 'h' 'i'
        assert(wire.length() == 4);
        assert(wire[0] == 0x81);
        assert(wire[1] == 0x02);
        assert(wire[2] == 'h' && wire[3] == 'i');

        auto dec = Ws::decode(wire.slice());
        assert(dec.ok());
        assert(dec.unwrap().consumed == 4);
        assert(dec.unwrap().frame.fin);
        assert(dec.unwrap().frame.op == Ws::Opcode::text);
        assert(dec.unwrap().frame.payload.length() == 2);
        assert(dec.unwrap().frame.payload[0] == 'h');
        assert(dec.unwrap().frame.payload[1] == 'i');
    }

    Trace("Websocket masked client frame") {
        u8 payload[] = {'P', 'I', 'N', 'G'};
        Ws::Frame f;
        f.fin = true;
        f.op = Ws::Opcode::binary;
        f.payload = Slice<const u8>{payload, 4};

        u8 mask[4] = {0xa5, 0x5a, 0x12, 0x34};
        Vec<u8, Mdefault> wire;
        Ws::encode(wire, f, true, mask);
        // header (2 bytes) + mask (4 bytes) + masked payload (4 bytes)
        assert(wire.length() == 10);
        assert(wire[0] == 0x82); // FIN | binary
        assert((wire[1] & 0x80) != 0);
        assert((wire[1] & 0x7f) == 4);

        // Masked payload must not be plaintext.
        assert(!(wire[6] == 'P' && wire[7] == 'I' && wire[8] == 'N' && wire[9] == 'G'));

        auto dec = Ws::decode(wire.slice());
        assert(dec.ok());
        assert(dec.unwrap().consumed == 10);
        assert(dec.unwrap().frame.payload.length() == 4);
        // After decode, the slice is unmasked back to plaintext.
        assert(dec.unwrap().frame.payload[0] == 'P');
        assert(dec.unwrap().frame.payload[1] == 'I');
        assert(dec.unwrap().frame.payload[2] == 'N');
        assert(dec.unwrap().frame.payload[3] == 'G');
    }

    Trace("Websocket medium frame (126 length escape)") {
        u8 payload[200];
        for(u64 i = 0; i < 200; i++) payload[i] = static_cast<u8>(i);
        Ws::Frame f;
        f.fin = true;
        f.op = Ws::Opcode::binary;
        f.payload = Slice<const u8>{payload, 200};

        Vec<u8, Mdefault> wire;
        Ws::encode(wire, f, false);
        assert(wire.length() == 4 + 200);
        assert(wire[0] == 0x82);
        assert(wire[1] == 126);
        assert(wire[2] == 0); // length high byte
        assert(wire[3] == 200);

        auto dec = Ws::decode(wire.slice());
        assert(dec.ok());
        assert(dec.unwrap().frame.payload.length() == 200);
        for(u64 i = 0; i < 200; i++) {
            assert(dec.unwrap().frame.payload[i] == static_cast<u8>(i));
        }
    }

    Trace("Websocket need_more on short input") {
        u8 partial[1] = {0x81};
        u8 buf[1];
        buf[0] = partial[0];
        auto dec = Ws::decode(Slice<u8>{buf, 1});
        assert(!dec.ok());
        assert(dec.unwrap_err() == "need_more"_v);
    }

    Trace("Websocket Sec-WebSocket-Accept RFC 6455 §1.3 vector") {
        // RFC example: key "dGhlIHNhbXBsZSBub25jZQ==" → accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
        auto accept = Ws::derive_accept<Mdefault>("dGhlIHNhbXBsZSBub25jZQ=="_v);
        assert(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="_v);
    }

    Trace("Websocket send/recv over Byte_Stream") {
        // Loopback through Memory_Stream: we send a masked client frame, then
        // feed the same wire bytes back into the recv side and decode.
        Net::Memory_Stream wire;

        u8 payload[] = {'P', 'O', 'N', 'G'};
        Ws::Frame f;
        f.fin = true;
        f.op = Ws::Opcode::binary;
        f.payload = Slice<const u8>{payload, 4};

        u8 mask[4] = {0x11, 0x22, 0x33, 0x44};
        auto sent = Ws::send_frame(wire, f, true, mask);
        assert(sent.ok());
        // 2 header + 4 mask + 4 masked payload.
        assert(sent.unwrap() == 10);

        // Splice the encoded bytes from the outbound side back into the
        // inbound side to simulate a peer echoing them.
        wire.inject(wire.sent());

        Vec<u8, Mdefault> rx_buf;
        u64 consumed = 0;
        auto frame = Ws::recv_frame_into(wire, rx_buf, consumed);
        assert(frame.ok());
        assert(consumed == 10);
        assert(frame.unwrap().fin);
        assert(frame.unwrap().op == Ws::Opcode::binary);
        assert(frame.unwrap().payload.length() == 4);
        assert(frame.unwrap().payload[0] == 'P');
        assert(frame.unwrap().payload[1] == 'O');
        assert(frame.unwrap().payload[2] == 'N');
        assert(frame.unwrap().payload[3] == 'G');
    }

    return 0;
}
