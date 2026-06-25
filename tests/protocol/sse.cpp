#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/sse.h>

namespace Sse = Protocol::Sse;
namespace Http = Protocol::Http;

i32 main() {
    Test test{"empty"_v};

    Trace("SSE single event") {
        Net::Memory_Stream wire;
        wire.inject("a\r\ndata: hi\n\n\r\n0\r\n\r\n"_v);

        Sse::Reader reader{wire};
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().event == "message"_v);
        assert(msg.unwrap().data == "hi"_v);
        assert(!msg.unwrap().done);

        auto eof = reader.next();
        assert(!eof.ok());
        assert(eof.unwrap_err() == "sse_eof"_v);
    }

    Trace("SSE event name") {
        Net::Memory_Stream wire;
        wire.inject("d\r\nevent: start\n\r\n"
                    "a\r\ndata: hi\n\n\r\n"
                    "0\r\n\r\n"_v);

        Sse::Reader reader{wire};
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().event == "start"_v);
        assert(msg.unwrap().data == "hi"_v);
    }

    Trace("SSE multi-line data") {
        Net::Memory_Stream wire;
        wire.inject("19\r\ndata: line1\ndata: line2\n\n\r\n0\r\n\r\n"_v);

        Sse::Reader reader{wire};
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().data == "line1\nline2"_v);
    }

    Trace("SSE [DONE] terminator") {
        Net::Memory_Stream wire;
        wire.inject("e\r\ndata: [DONE]\n\n\r\n0\r\n\r\n"_v);

        Sse::Reader reader{wire};
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().done);
    }

    Trace("SSE fragmented chunks") {
        Net::Memory_Stream wire;
        wire.inject("a\r\ndata: "_v);
        wire.inject("hi\n\n\r\n0\r\n\r\n"_v);

        Sse::Reader reader{wire};
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().data == "hi"_v);
    }

    Trace("SSE comment ignored") {
        Net::Memory_Stream wire;
        wire.inject("13\r\n: comment\ndata: x\n\n\r\n0\r\n\r\n"_v);

        Sse::Reader reader{wire};
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().data == "x"_v);
    }

    Trace("SSE open helper") {
        Net::Memory_Stream wire;
        wire.inject("HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "a\r\ndata: hi\n\n\r\n"
                    "0\r\n\r\n"_v);

        Http::Request req;
        req.method = "GET"_v;
        req.path = "/v1/messages"_v;
        req.host = "api.anthropic.com"_v;

        auto opened = Sse::open(wire, req);
        assert(opened.ok());

        auto& reader = opened.unwrap();
        auto msg = reader.next();
        assert(msg.ok());
        assert(msg.unwrap().data == "hi"_v);

        // The request must have been written to the stream.
        auto outgoing = wire.sent();
        assert(outgoing.length() > 0);
        String_View req_text{outgoing.data(), outgoing.length()};
        assert(req_text.sub(0, 3) == "GET"_v);
    }

    Trace("SSE open rejects non-SSE content type") {
        Net::Memory_Stream wire;
        wire.inject("HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "\r\n"_v);

        Http::Request req;
        req.method = "GET"_v;
        req.path = "/"_v;
        req.host = "x"_v;

        auto opened = Sse::open(wire, req);
        assert(!opened.ok());
        assert(opened.unwrap_err() == "sse_not_event_stream"_v);
    }

    return 0;
}
