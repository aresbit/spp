#pragma once

// Binance Spot market-data WebSocket subscription helper.
//
// Builds on Ws_Client to manage subscription state. A single connection can
// host many streams (e.g. "btcusdt@ticker", "ethusdt@depth@100ms"). Binance
// expects subscription/unsubscription messages of the form
//   {"method": "SUBSCRIBE", "params": ["btcusdt@trade"], "id": <n>}
// and the server replies with `{"result": null, "id": <n>}` for the
// administrative messages, followed by streaming data frames.

#include <binance/ws_client.h>

namespace spp::App::Binance {

inline const String_View market_stream_host = "stream.binance.com"_v;
inline constexpr u16 market_stream_wss_port = 9443;

template<typename S>
    requires Net::Byte_Stream<S>
struct Market_Stream {
    Ws_Client<S> ws;
    u32 next_id = 1;
    Vec<String<Mdefault>, Mdefault> subscriptions;

    explicit Market_Stream(S& s) noexcept : ws(s) {
    }

    [[nodiscard]] Result<u64, String_View> open(String_View host = market_stream_host,
                                                 String_View path = "/ws"_v) noexcept {
        return ws.handshake(host, path);
    }

    // Subscribe to one or more streams. Binance interprets `params` as the
    // exact stream names (lowercase symbol + suffix).
    [[nodiscard]] Result<u64, String_View>
    subscribe(Slice<const String_View> streams) noexcept {
        return send_admin_("SUBSCRIBE"_v, streams, true);
    }

    [[nodiscard]] Result<u64, String_View>
    unsubscribe(Slice<const String_View> streams) noexcept {
        return send_admin_("UNSUBSCRIBE"_v, streams, false);
    }

    // Pull the next data message (raw bytes). Caller parses the JSON into
    // a typed struct via spp::Json::parse_result.
    [[nodiscard]] Result<Vec<u8, Mdefault>, String_View> recv() noexcept {
        return ws.recv_message();
    }

private:
    [[nodiscard]] Result<u64, String_View>
    send_admin_(String_View method, Slice<const String_View> streams, bool track) noexcept {
        // Hand-rolled JSON builder; the request is small enough that going
        // through Json::Builder is overkill (and Builder doesn't render the
        // "id" as an integer literal without a struct + reflection trip).
        Vec<u8, Mdefault> buf;
        auto push_lit = [&buf](const char* s) {
            while(*s) buf.push(static_cast<u8>(*s++));
        };
        auto push_sv = [&buf](String_View sv) {
            for(u64 i = 0; i < sv.length(); i++) buf.push(sv[i]);
        };

        push_lit("{\"method\":\"");
        push_sv(method);
        push_lit("\",\"params\":[");
        for(u64 i = 0; i < streams.length(); i++) {
            if(i > 0) push_lit(",");
            buf.push('"');
            push_sv(streams[i]);
            buf.push('"');
            if(track) {
                subscriptions.push(streams[i].string<Mdefault>());
            }
        }
        push_lit("],\"id\":");
        {
            u8 id_buf[16];
            i32 n = Libc::snprintf(id_buf, sizeof(id_buf), "%u", next_id++);
            for(i32 j = 0; j < n; j++) buf.push(id_buf[j]);
        }
        push_lit("}");

        return ws.send_text(String_View{buf.data(), buf.length()});
    }
};

} // namespace spp::App::Binance
