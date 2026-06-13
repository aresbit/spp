#pragma once

// Binance Spot User Data Stream.
//
// Lifecycle:
//   1. POST /api/v3/userDataStream  (X-MBX-APIKEY only, no signature) → listenKey
//   2. wss://stream.binance.com:9443/ws/<listenKey>  → opens the user stream
//   3. PUT /api/v3/userDataStream?listenKey=<key>   every <30 minutes to renew
//   4. DELETE /api/v3/userDataStream?listenKey=<key> on shutdown
//
// The HTTP REST calls happen on a separate REST connection from the WS
// connection — they share the limiter but are otherwise decoupled.
//
// Once subscribed, server emits:
//   - outboundAccountPosition  (balance changes)
//   - balanceUpdate            (deposit/withdraw)
//   - executionReport          (order updates)
//
// The wire shape of each event is documented at
// https://github.com/binance/binance-spot-api-docs/blob/master/user-data-stream.md.
// We provide raw byte access here; typed parsing is up to the caller (the
// payloads change frequently).

#include <binance/api.h>
#include <binance/rate_limiter.h>
#include <binance/ws_client.h>

namespace spp::App::Binance {

struct Listen_Key {
    String<Mdefault> listenKey;
};

} // namespace spp::App::Binance

namespace spp {

SPP_NAMED_RECORD(App::Binance::Listen_Key, "Listen_Key", SPP_FIELD(listenKey));

} // namespace spp

namespace spp::App::Binance {

// REST: POST /api/v3/userDataStream → {"listenKey": "..."}.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Listen_Key, String_View>
create_listen_key(S& stream, String_View host, String_View api_key, Rate_Limiter& limiter,
                  i64 now_ms_value) noexcept {
    Protocol::Http::Request<Mdefault> req;
    req.method = "POST"_v;
    req.path = "/api/v3/userDataStream"_v;
    req.host = host;
    req.headers.push(Protocol::Http::Header{"X-MBX-APIKEY"_v, api_key});
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Listen_Key, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    String_View body{fetched.unwrap().response.body.data(),
                     fetched.unwrap().response.body.length()};
    return Json::parse_result<Listen_Key>(body);
}

// REST: PUT /api/v3/userDataStream?listenKey=<key>  every <30min.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<u64, String_View>
keepalive_listen_key(S& stream, String_View host, String_View api_key, String_View listen_key,
                     Rate_Limiter& limiter, i64 now_ms_value) noexcept {
    auto query = "listenKey="_v.append<Mdefault>(listen_key);
    auto path = "/api/v3/userDataStream?"_v.append<Mdefault>(query.view());
    Protocol::Http::Request<Mdefault> req;
    req.method = "PUT"_v;
    req.path = path.view();
    req.host = host;
    req.headers.push(Protocol::Http::Header{"X-MBX-APIKEY"_v, api_key});
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<u64, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return Result<u64, String_View>::ok(0);
}

// REST: DELETE /api/v3/userDataStream?listenKey=<key>.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<u64, String_View>
close_listen_key(S& stream, String_View host, String_View api_key, String_View listen_key,
                 Rate_Limiter& limiter, i64 now_ms_value) noexcept {
    auto query = "listenKey="_v.append<Mdefault>(listen_key);
    auto path = "/api/v3/userDataStream?"_v.append<Mdefault>(query.view());
    Protocol::Http::Request<Mdefault> req;
    req.method = "DELETE"_v;
    req.path = path.view();
    req.host = host;
    req.headers.push(Protocol::Http::Header{"X-MBX-APIKEY"_v, api_key});
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<u64, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return Result<u64, String_View>::ok(0);
}

// WS session bound to a previously-created listenKey. Construct from a TLS
// Byte_Stream that's already connected to stream.binance.com:9443.
template<typename S>
    requires Net::Byte_Stream<S>
struct User_Stream {
    Ws_Client<S> ws;

    explicit User_Stream(S& s) noexcept : ws(s) {
    }

    [[nodiscard]] Result<u64, String_View>
    open(String_View listen_key, String_View host = market_stream_host) noexcept {
        auto path = "/ws/"_v.append<Mdefault>(listen_key);
        return ws.handshake(host, path.view());
    }

    [[nodiscard]] Result<Vec<u8, Mdefault>, String_View> recv_event() noexcept {
        return ws.recv_message();
    }
};

} // namespace spp::App::Binance
