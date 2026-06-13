#pragma once

// Single-connection WebSocket session over any Net::Byte_Stream.
//
// Handles:
//   - RFC 6455 client handshake (HTTP/1.1 GET + Upgrade)
//   - Sec-WebSocket-Accept verification against client_key
//   - Per-frame client masking (mandatory by spec)
//   - Continuation frames (FIN=0 then op=cont) reassembled into one message
//   - Server PING → auto-PONG with matching payload
//   - Server PONG → silently swallowed
//   - Server CLOSE → reply with matching CLOSE and mark stream closed
//
// Caller drives the loop via `recv_message()` which only yields data frames
// (text / binary). Control frames are absorbed internally so the protocol
// stays correct without bothering the higher-level subscription code.

#include <spp/core/rng.h>
#include <spp/crypto/encode.h>
#include <spp/io/stream.h>
#include <spp/protocol/http.h>
#include <spp/protocol/websocket.h>

namespace spp::App::Binance {

template<typename S>
    requires Net::Byte_Stream<S>
struct Ws_Client {
    S& stream;
    RNG::Stream rng;
    Vec<u8, Mdefault> rx_buffer;
    bool handshake_done = false;
    bool peer_closed = false;

    explicit Ws_Client(S& s) noexcept : stream(s), rng(0) {
        // Seed RNG from the address of the stream + a few bytes of rng default
        // state. Good enough for client mask generation (RFC 6455 only
        // requires per-frame randomness, not crypto-grade entropy).
        rng.seed(reinterpret_cast<u64>(&s) ^ static_cast<u64>(rx_buffer.length()));
    }

    // Reset the protocol state so a fresh handshake can be issued on the
    // same Ws_Client instance — used by Ws_Session after the underlying
    // TLS reconnects. The receive buffer is dropped (mid-frame bytes from
    // the dead session are unsafe to interpret), and the closed/done
    // flags are cleared. RNG seed is NOT re-rolled; per-frame mask
    // randomness is fine continuing from where it left off.
    void reset() noexcept {
        rx_buffer = Vec<u8, Mdefault>{};
        handshake_done = false;
        peer_closed = false;
    }

    // Perform the client-side handshake. `host` is the `Host:` header, `path`
    // is the URL path (Binance: "/ws/<stream>" or "/ws/<listenKey>"). After
    // a successful return, the stream is in WS mode and recv_message /
    // send_text / send_binary are usable.
    [[nodiscard]] Result<u64, String_View> handshake(String_View host,
                                                      String_View path) noexcept {
        if(handshake_done) return Result<u64, String_View>::err("ws_already_open"_v);

        // 16 random bytes → base64 → 24 chars including padding.
        u8 nonce[16];
        for(u64 i = 0; i < 16; i++) nonce[i] = static_cast<u8>(rng() & 0xff);
        auto client_key = Crypto::base64_encode<Mdefault>(Slice<const u8>{nonce, 16});

        Protocol::Http::Request<Mdefault> req;
        req.method = "GET"_v;
        req.path = path;
        req.host = host;
        req.headers.push(Protocol::Http::Header{"Upgrade"_v, "websocket"_v});
        req.headers.push(Protocol::Http::Header{"Connection"_v, "Upgrade"_v});
        req.headers.push(Protocol::Http::Header{"Sec-WebSocket-Key"_v, client_key.view()});
        req.headers.push(Protocol::Http::Header{"Sec-WebSocket-Version"_v, "13"_v});

        auto wire = req.template to_bytes<Mdefault>();
        auto sent = stream.send_all_result(wire.slice());
        if(!sent.ok()) return Result<u64, String_View>::err(spp::move(sent.unwrap_err()));

        // Read until \r\n\r\n (header terminator). WS upgrade response has no
        // body, so we don't need Http::fetch's body machinery.
        u8 chunk[2048];
        u64 headers_end = 0;
        for(;;) {
            auto got = stream.recv_result(Slice<u8>{chunk, sizeof(chunk)});
            if(!got.ok()) return Result<u64, String_View>::err(spp::move(got.unwrap_err()));
            u64 n = got.unwrap();
            if(n == 0) return Result<u64, String_View>::err("ws_eof_in_handshake"_v);
            for(u64 i = 0; i < n; i++) rx_buffer.push(chunk[i]);
            u64 search_from = rx_buffer.length() - n > 3 ? rx_buffer.length() - n - 3 : 0;
            for(u64 i = search_from; i + 3 < rx_buffer.length(); i++) {
                if(rx_buffer[i] == '\r' && rx_buffer[i + 1] == '\n' &&
                   rx_buffer[i + 2] == '\r' && rx_buffer[i + 3] == '\n') {
                    headers_end = i + 4;
                    break;
                }
            }
            if(headers_end > 0) break;
        }

        auto parsed = Protocol::Http::parse_response<Mdefault>(
            Slice<const u8>{rx_buffer.data(), headers_end});
        if(!parsed.ok()) return Result<u64, String_View>::err(spp::move(parsed.unwrap_err()));
        if(parsed.unwrap().status_code != 101) {
            return Result<u64, String_View>::err("ws_handshake_bad_status"_v);
        }

        auto accept = parsed.unwrap().find_header("Sec-WebSocket-Accept"_v);
        if(!accept.ok()) {
            return Result<u64, String_View>::err("ws_handshake_no_accept"_v);
        }
        auto expected = Protocol::Websocket::derive_accept<Mdefault>(client_key.view());
        if(*accept != expected.view()) {
            return Result<u64, String_View>::err("ws_handshake_accept_mismatch"_v);
        }

        // Compact rx_buffer: any bytes after the header terminator are the
        // start of WS frames (servers can pipeline frames immediately).
        Vec<u8, Mdefault> rest;
        for(u64 i = headers_end; i < rx_buffer.length(); i++) rest.push(rx_buffer[i]);
        rx_buffer = spp::move(rest);

        handshake_done = true;
        return Result<u64, String_View>::ok(0);
    }

    [[nodiscard]] Result<u64, String_View> send_text(String_View text) noexcept {
        return send_frame_(Protocol::Websocket::Opcode::text,
                           Slice<const u8>{text.data(), text.length()});
    }

    [[nodiscard]] Result<u64, String_View> send_binary(Slice<const u8> bytes) noexcept {
        return send_frame_(Protocol::Websocket::Opcode::binary, bytes);
    }

    // Status code 1000 = normal closure. Servers reply with matching close.
    [[nodiscard]] Result<u64, String_View> send_close(u16 code = 1000) noexcept {
        u8 payload[2];
        payload[0] = static_cast<u8>((code >> 8) & 0xff);
        payload[1] = static_cast<u8>(code & 0xff);
        return send_frame_(Protocol::Websocket::Opcode::close,
                           Slice<const u8>{payload, 2});
    }

    // Pull the next data message (text or binary). Returns the assembled
    // payload bytes — caller treats them as text or binary based on
    // application context (Binance always sends JSON text frames).
    [[nodiscard]] Result<Vec<u8, Mdefault>, String_View> recv_message() noexcept {
        if(!handshake_done) return Result<Vec<u8, Mdefault>, String_View>::err("ws_not_open"_v);
        if(peer_closed) return Result<Vec<u8, Mdefault>, String_View>::err("ws_closed"_v);

        Vec<u8, Mdefault> message;
        Protocol::Websocket::Opcode first_op = Protocol::Websocket::Opcode::cont;
        bool started = false;

        for(;;) {
            // Try to decode a frame from the buffer; on need_more, read more.
            while(true) {
                auto dec = Protocol::Websocket::decode(rx_buffer.slice());
                if(dec.ok()) {
                    auto& frame = dec.unwrap().frame;
                    u64 consumed = dec.unwrap().consumed;

                    // Handle control frames in-place.
                    if(frame.op == Protocol::Websocket::Opcode::ping) {
                        auto pong_rc =
                            send_frame_(Protocol::Websocket::Opcode::pong, frame.payload);
                        // Frame was consumed; drop from rx_buffer regardless of
                        // pong success so we don't reprocess on next iteration.
                        consume_(consumed);
                        if(!pong_rc.ok()) {
                            return Result<Vec<u8, Mdefault>, String_View>::err(
                                spp::move(pong_rc.unwrap_err()));
                        }
                        break; // loop back to try decoding the next frame
                    }
                    if(frame.op == Protocol::Websocket::Opcode::pong) {
                        consume_(consumed);
                        break;
                    }
                    if(frame.op == Protocol::Websocket::Opcode::close) {
                        peer_closed = true;
                        // Echo the close to be polite; ignore failures.
                        static_cast<void>(send_frame_(Protocol::Websocket::Opcode::close,
                                                       frame.payload));
                        consume_(consumed);
                        return Result<Vec<u8, Mdefault>, String_View>::err("ws_closed"_v);
                    }

                    // Data frame (text/binary/continuation).
                    if(!started) {
                        if(frame.op == Protocol::Websocket::Opcode::cont) {
                            consume_(consumed);
                            return Result<Vec<u8, Mdefault>, String_View>::err(
                                "ws_unexpected_continuation"_v);
                        }
                        first_op = frame.op;
                        started = true;
                    }
                    for(u64 i = 0; i < frame.payload.length(); i++) {
                        message.push(frame.payload[i]);
                    }
                    bool fin = frame.fin;
                    consume_(consumed);
                    if(fin) {
                        (void)first_op; // could re-expose if caller wants the type
                        return Result<Vec<u8, Mdefault>, String_View>::ok(spp::move(message));
                    }
                    break; // continue accumulating from next frame
                }
                if(dec.unwrap_err() != "need_more"_v) {
                    return Result<Vec<u8, Mdefault>, String_View>::err(
                        spp::move(dec.unwrap_err()));
                }
                // Pull more bytes off the wire.
                u8 chunk[4096];
                auto got = stream.recv_result(Slice<u8>{chunk, sizeof(chunk)});
                if(!got.ok()) {
                    return Result<Vec<u8, Mdefault>, String_View>::err(
                        spp::move(got.unwrap_err()));
                }
                u64 n = got.unwrap();
                if(n == 0) {
                    return Result<Vec<u8, Mdefault>, String_View>::err("ws_eof"_v);
                }
                for(u64 i = 0; i < n; i++) rx_buffer.push(chunk[i]);
                // Re-attempt decode with the larger buffer.
            }
        }
    }

private:
    void consume_(u64 n) noexcept {
        Vec<u8, Mdefault> rest;
        rest.reserve(rx_buffer.length() > n ? rx_buffer.length() - n : 0);
        for(u64 i = n; i < rx_buffer.length(); i++) rest.push(rx_buffer[i]);
        rx_buffer = spp::move(rest);
    }

    [[nodiscard]] Result<u64, String_View>
    send_frame_(Protocol::Websocket::Opcode op, Slice<const u8> payload) noexcept {
        Protocol::Websocket::Frame f;
        f.fin = true;
        f.op = op;
        f.payload = payload;
        u8 mask[4];
        for(u64 i = 0; i < 4; i++) mask[i] = static_cast<u8>(rng() & 0xff);
        return Protocol::Websocket::send_frame(stream, f, true, mask);
    }
};

} // namespace spp::App::Binance
