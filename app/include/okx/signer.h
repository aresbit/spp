#pragma once

// OKX V5 REST signer.
//
// OKX's signing differs from Binance Spot:
//
//   Binance: signature = HEX(HMAC-SHA256(secret, query_string))
//            passed as `signature=...` query parameter, alongside an
//            `X-MBX-APIKEY` header.
//
//   OKX:     prehash   = TS + METHOD + PATH + BODY
//            signature = BASE64(HMAC-SHA256(secret, prehash))
//            passed via the OK-ACCESS-SIGN header, alongside four other
//            OK-ACCESS-* headers carrying key, timestamp, passphrase.
//
// The signing string is timestamp-prefixed so a captured request can't be
// replayed past the server's 30s validity window; the passphrase is a
// per-key shared secret set when the API key was created.
//
// `Signed_Request` owns the heap-backed buffers (timestamp string,
// signature, body, path) so the `Request`'s String_Views remain valid
// for the lifetime of the wrapper.  This mirrors what
// `Exchange::Binance::signed_request_full` does.

#include <spp/core/base.h>
#include <spp/crypto/encode.h>
#include <spp/crypto/hmac.h>
#include <spp/protocol/http.h>

#include <okx/clock.h>

namespace spp::App::Okx {

inline const String_View k_mainnet_host = "www.okx.com"_v;
inline const String_View k_aws_host     = "aws.okx.com"_v;

// Authentication material the signer needs.  All four strings are
// caller-owned — the signer borrows views into them and never copies.
struct Signer_Config {
    String_View api_key;
    String_View api_secret;
    String_View passphrase;
    // Set true to add `x-simulated-trading: 1` so the request hits OKX's
    // testnet ("Demo Trading") instead of production.
    bool simulated_trading = false;
};

// Owned counterpart of a signed `Request`. `request.path` /
// `request.body` borrow from the owned strings here, so callers must
// keep the wrapper alive while serializing.
template<Allocator A = Mdefault>
struct Signed_Request {
    String<A> ts_owned;
    String<A> path_owned;
    String<A> body_owned;
    String<A> sign_owned;
    Protocol::Http::Request<A> request;
};

// Build a signed REST request.
//
//   method: "GET" / "POST" / "DELETE"
//   path:   "/api/v5/trade/order" (without query string)
//   query:  for GET — the canonical "k1=v1&k2=v2" form; appended to path
//                     (becomes part of RequestPath in the prehash).
//           for POST — usually empty; OKX puts params in the JSON body.
//   body:   JSON body for POST; empty for GET.
//
// `now_ms` is passed through so unit tests can pin the timestamp.
template<Allocator A = Mdefault>
[[nodiscard]] inline Signed_Request<A>
signed_request_full(String_View method, String_View path, String_View query,
                    String_View body, String_View host,
                    const Signer_Config& cfg, i64 now_ms_value) noexcept {
    Signed_Request<A> out;

    // 1. Final RequestPath = path[?query] — both the request line AND
    //    the prehash use this exact value, so they have to agree.
    if(query.length() > 0) {
        auto p1 = path.template append<A>("?"_v);
        out.path_owned = p1.view().template append<A>(query);
    } else {
        out.path_owned = path.template string<A>();
    }
    out.body_owned = body.template string<A>();
    out.ts_owned   = iso8601_ms(now_ms_value);

    // 2. prehash = ts + method + requestPath + body
    auto p1 = out.ts_owned.view().template append<A>(method);
    auto p2 = p1.view().template append<A>(out.path_owned.view());
    auto prehash = p2.view().template append<A>(out.body_owned.view());

    auto digest = Crypto::HMAC_SHA256::sign(
        Slice<const u8>{cfg.api_secret.data(), cfg.api_secret.length()},
        Slice<const u8>{prehash.data(), prehash.length()});

    out.sign_owned = Crypto::base64_encode<A>(
        Slice<const u8>{digest.bytes, Crypto::HMAC_SHA256::digest_size});

    // 3. Build the HTTP request.
    out.request.method = method;
    out.request.host = host;
    out.request.path = out.path_owned.view();
    if(out.body_owned.length() > 0) {
        out.request.body = Slice<const u8>{
            out.body_owned.data(), out.body_owned.length()};
        out.request.headers.push(Protocol::Http::Header{
            "Content-Type"_v, "application/json"_v});
    }
    out.request.headers.push(Protocol::Http::Header{
        "OK-ACCESS-KEY"_v, cfg.api_key});
    out.request.headers.push(Protocol::Http::Header{
        "OK-ACCESS-SIGN"_v, out.sign_owned.view()});
    out.request.headers.push(Protocol::Http::Header{
        "OK-ACCESS-TIMESTAMP"_v, out.ts_owned.view()});
    out.request.headers.push(Protocol::Http::Header{
        "OK-ACCESS-PASSPHRASE"_v, cfg.passphrase});
    if(cfg.simulated_trading) {
        out.request.headers.push(Protocol::Http::Header{
            "x-simulated-trading"_v, "1"_v});
    }
    return out;
}

// Convenience: just the signature value (no Request).  Useful for unit
// tests and for the WS-private login op (which also signs with HMAC but
// uses a UNIX-seconds timestamp + a fixed `/users/self/verify` path).
template<Allocator A = Mdefault>
[[nodiscard]] inline String<A>
sign(String_View ts, String_View method, String_View request_path,
     String_View body, String_View api_secret) noexcept {
    auto p1 = ts.template append<A>(method);
    auto p2 = p1.view().template append<A>(request_path);
    auto prehash = p2.view().template append<A>(body);
    auto digest = Crypto::HMAC_SHA256::sign(
        Slice<const u8>{api_secret.data(), api_secret.length()},
        Slice<const u8>{prehash.data(), prehash.length()});
    return Crypto::base64_encode<A>(
        Slice<const u8>{digest.bytes, Crypto::HMAC_SHA256::digest_size});
}

} // namespace spp::App::Okx
