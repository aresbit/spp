# Binance Integration

Date: 2026-05-30
Scope: how to wire `spp::Exchange::Binance` + `spp::Protocol::Http` / `Websocket`
into a Binance Spot REST + market-data pipeline.

## Status

| Layer | Provided | Gap |
|---|---|---|
| SHA-256 / SHA-1 / HMAC-SHA256 | `include/spp/crypto/{sha256,sha1,hmac}.h` | — |
| Hex / Base64 / URL percent-encode | `include/spp/crypto/encode.h` | — |
| Blocking TCP client | `Net::Tcp_Client` in `include/spp/io/net.h` | non-blocking / async variant |
| `Net::Byte_Stream` concept + virtual `Stream` interface | `include/spp/io/stream.h` | — |
| In-memory test stream | `Net::Memory_Stream` | — |
| HTTP/1.1 request build + parse (generic over `Byte_Stream`) | `include/spp/protocol/http.h` | chunked transfer encoding |
| WebSocket frame codec (RFC 6455) + stream helpers | `include/spp/protocol/websocket.h` | client handshake driver, ping/pong scheduler |
| Binance Spot signer (HMAC-SHA256 query signing, `X-MBX-APIKEY` plumbing) | `include/spp/exchange/binance.h` | typed Spot/Futures request/response wrappers |
| **TLS (HTTPS / WSS) — mbedTLS adapter** | `include/spp/ext/tls_mbedtls.h` (Linux-only, opt-in via `SPP_TLS=1`) | non-blocking / async wrapper |
| Order book maintenance, user data stream, time sync | — | Application-layer concerns; out of scope for the protocol foundation. |

## TLS options

Binance only accepts HTTPS (`api.binance.com:443`) and WSS (`stream.binance.com:9443`).
Plain HTTP / WS will be refused. Pick the option that matches your deployment:

### Option A — native `Ext::Tls_Mbedtls_Stream` (recommended for embedded / standalone)

`include/spp/ext/tls_mbedtls.h` ships a `Byte_Stream`-satisfying adapter over
mbedTLS 3.x. Verify-required PKI by default, system trust-store auto-probed
(Debian/Ubuntu, RHEL/Fedora, Alpine), SNI set, TLS 1.2 minimum.

Build:

```sh
sudo apt install -y libmbedtls-dev   # Debian/Ubuntu — mbedTLS 3.6+
SPP_TLS=1 make test                   # enables tests/ext/ + links -lmbedtls -lmbedx509 -lmbedcrypto
```

Use:

```cpp
#include <spp/ext/tls_mbedtls.h>

spp::Ext::Tls_Mbedtls_Stream tls;
auto _ = tls.connect_result("api.binance.com"_v, 443);
auto fetched = spp::Protocol::Http::fetch(tls, request);  // same call as plain TCP
```

Because the adapter satisfies `Net::Byte_Stream`, every Http / Websocket /
Binance signer call site that was written against `Tcp_Client` works with
`Tls_Mbedtls_Stream` unchanged.

### Option B — sidecar (envoy / stunnel) for HFT

For latency-critical setups co-locate a TLS terminator (envoy / nginx /
stunnel) in the same network namespace and have SPP talk to it over loopback
or a Unix domain socket. The HTTP/1.1 bytes SPP emits are spec-conformant and
pass through any TLS-terminating proxy transparently.

```ini
# /etc/stunnel/binance.conf
[binance-rest]
client = yes
accept = 127.0.0.1:8443
connect = api.binance.com:443
```

Then `Tcp_Client::connect_result("127.0.0.1"_v, 8443)` and set
`Request::host = "api.binance.com"_v` so the inner `Host:` line stays correct.

### Option C — application-supplied TLS Stream

Any type that satisfies `Net::Byte_Stream` slots into Http / Websocket / the
Binance signer transparently. If you already link BoringSSL / OpenSSL /
Schannel and prefer those over mbedTLS, write a 60-line adapter mirroring
`include/spp/ext/tls_mbedtls.h` and you are done.

## End-to-end signed request shape

```cpp
using namespace spp;

Exchange::Binance::Query_Builder<Mdefault> q;
q.add("symbol"_v,       "LTCBTC"_v);
q.add("side"_v,         "BUY"_v);
q.add("type"_v,         "LIMIT"_v);
q.add("timeInForce"_v,  "GTC"_v);
q.add("quantity"_v,     "1"_v);
q.add("price"_v,        "0.1"_v);
q.add("recvWindow"_v,   "5000"_v);
q.add("timestamp"_v,    "1499827319559"_v);  // application-provided

auto sr = Exchange::Binance::signed_request_full<Mdefault>(
    "POST"_v, "/api/v3/order"_v, q.view(),
    Exchange::Binance::k_spot_host, api_key, api_secret);

Ext::Tls_Mbedtls_Stream tls;
auto _ = tls.connect_result(Exchange::Binance::k_spot_host, 443);
auto fetched = Protocol::Http::fetch(tls, sr.request);
// fetched.unwrap().response.status_code  / .body  -> parse via spp/reflection/json.h
```

The same code with `Net::Tcp_Client tcp` instead of `Tls_Mbedtls_Stream tls`
gets you the sidecar path — both satisfy `Net::Byte_Stream` so `Http::fetch`
doesn't care which is in play.

## What's NOT in this commit

- Async (coroutine-style) HTTP / TLS clients. Today's adapters are blocking;
  wrapping them in an `Async::Task` on top of the existing `Pool` is the next
  natural step.
- Chunked transfer decoding. Binance Spot REST always returns `Content-Length`.
- Typed Binance Spot/Futures models. We left those to the consuming
  application: `reflection/json.h` + `reflection/reflect.h` already covers the
  marshalling once you declare the response structs.

## Going-live checklist

Before pointing this stack at real funds:

1. Pick a TLS path and validate against testnet — `Ext::Tls_Mbedtls_Stream`
   (Option A) → `GET https://testnet.binance.vision/api/v3/time` should
   return 200. The same client connects to `api.binance.com:443` for production.
2. Verify the Spot signer against a known-good Binance docs example (covered
   by `tests/exchange/binance.cpp`).
3. Synchronize local clock to NTP. Binance rejects requests where
   `timestamp - serverTime > recvWindow`; serverTime is reachable through
   `/api/v3/time` and must be polled regularly.
4. Wire rate-limit headers (`X-MBX-USED-WEIGHT-*`, `X-MBX-ORDER-COUNT-*`)
   into your throttling layer.
5. Pin the testnet endpoint (`testnet.binance.vision`) for the full
   integration test before any production endpoint touches the wire.
