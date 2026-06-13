#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/http.h>

namespace Http = Protocol::Http;

i32 main() {
    Test test{"empty"_v};

    Trace("Http request build") {
        Http::Request req;
        req.method = "POST"_v;
        req.path = "/api/v3/order"_v;
        req.host = "api.binance.com"_v;
        req.headers.push(Http::Header{"X-MBX-APIKEY"_v, "key123"_v});
        u8 body[] = {'h', 'i'};
        req.body = Slice<const u8>{body, 2};

        auto wire = req.to_bytes<Mdefault>();
        String_View serialized{wire.data(), wire.length()};
        // Spot-check structural pieces. Exact byte equality is fragile across
        // header ordering choices, so check the surface that protocol cares
        // about.
        assert(serialized.length() > 0);
        assert(serialized.sub(0, 27) == "POST /api/v3/order HTTP/1.1"_v);
        bool has_host = false;
        bool has_apikey = false;
        bool has_content_length_2 = false;
        for(u64 i = 0; i + 23 < serialized.length(); i++) {
            if(serialized.sub(i, i + 23) == "Host: api.binance.com\r\n"_v) has_host = true;
            if(serialized.sub(i, i + 22) == "X-MBX-APIKEY: key123\r\n"_v) has_apikey = true;
        }
        for(u64 i = 0; i + 19 < serialized.length(); i++) {
            if(serialized.sub(i, i + 19) == "Content-Length: 2\r\n"_v) has_content_length_2 = true;
        }
        assert(has_host);
        assert(has_apikey);
        assert(has_content_length_2);
        // Body must follow the empty line.
        assert(serialized.sub(serialized.length() - 2, serialized.length()) == "hi"_v);
    }

    Trace("Http response parse") {
        String_View raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 19\r\n"
            "X-Mbx-Used-Weight: 1\r\n"
            "\r\n"
            "{\"serverTime\":1234}"_v;

        auto parsed = Http::parse_response<Mdefault>(Slice<const u8>{raw.data(), raw.length()});
        assert(parsed.ok());
        auto& resp = parsed.unwrap();
        assert(resp.status_code == 200);
        assert(resp.status_text == "OK"_v);

        auto ct = resp.find_header("Content-Type"_v);
        assert(ct.ok());
        assert(*ct == "application/json"_v);

        // Case-insensitive header lookup.
        auto cl = resp.find_header("content-length"_v);
        assert(cl.ok());
        assert(*cl == "19"_v);

        assert(resp.body.length() == 19);
        String_View body{resp.body.data(), resp.body.length()};
        assert(body == "{\"serverTime\":1234}"_v);
    }

    Trace("Http response truncated body") {
        String_View raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 100\r\n"
            "\r\n"
            "short"_v;
        auto parsed = Http::parse_response<Mdefault>(Slice<const u8>{raw.data(), raw.length()});
        assert(!parsed.ok());
        assert(parsed.unwrap_err() == "http_body_truncated"_v);
    }

    Trace("Http response no body") {
        String_View raw =
            "HTTP/1.1 204 No Content\r\n"
            "\r\n"_v;
        auto parsed = Http::parse_response<Mdefault>(Slice<const u8>{raw.data(), raw.length()});
        assert(parsed.ok());
        assert(parsed.unwrap().status_code == 204);
        assert(parsed.unwrap().status_text == "No Content"_v);
        assert(parsed.unwrap().body.length() == 0);
    }

    Trace("Http fetch over Memory_Stream (transport-agnostic round-trip)") {
        // Proves Http::fetch is templated on Net::Byte_Stream: no socket touched.
        Net::Memory_Stream wire;
        // Pre-seed the inbound channel with a complete HTTP/1.1 response.
        wire.inject(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 21\r\n"
            "\r\n"
            "{\"price\":\"42000.50\"}\n"_v);

        Http::Request req;
        req.method = "GET"_v;
        req.path = "/api/v3/ticker/price?symbol=BTCUSDT"_v;
        req.host = "api.binance.com"_v;

        auto fetched = Http::fetch(wire, req);
        assert(fetched.ok());
        auto& r = fetched.unwrap();
        assert(r.response.status_code == 200);
        assert(r.response.body.length() == 21);
        String_View body{r.response.body.data(), r.response.body.length()};
        assert(body == "{\"price\":\"42000.50\"}\n"_v);

        // The request bytes must have been emitted onto the stream's outgoing
        // half — anything wrapping this stream (TLS, sidecar) will see them.
        auto outgoing = wire.sent();
        assert(outgoing.length() > 0);
        String_View req_text{outgoing.data(), outgoing.length()};
        assert(req_text.sub(0, 4) == "GET "_v);
    }

    return 0;
}
