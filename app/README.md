# SPP App Layer — Binance Spot Integration

End-to-end Binance Spot REST client built on top of SPP. Combines the
foundations (TLS adapter, HTTP/1.1, signer) with the three things still
missing for production trading:

1. **`Time_Sync`** — periodic skew sampling against `GET /api/v3/time` so
   signed requests stay inside `recvWindow`.
2. **`Rate_Limiter`** — token-bucket accounting from `X-MBX-USED-WEIGHT-*`
   headers, plus 429/418 cooldown / IP-ban handling.
3. **Typed models + free API functions** — `Server_Time`, `Ticker_Price`,
   `Account_Info`, `Order_Request`, `Order_Response`, all marshalled via
   `spp::reflection::json`.

The `Client` facade in `binance/client.h` glues all of this on top of
`Ext::Tls_Mbedtls_Stream`, but the same free functions in `binance/api.h`
work over any `Net::Byte_Stream` — that's what the Memory_Stream-driven
unit tests exercise.

## Layout

```
app/
├── include/binance/
│   ├── clock.h           # now_ms()
│   ├── models.h          # typed Binance wire types + SPP_RECORD
│   ├── rate_limiter.h    # token bucket + 429/418
│   ├── time_sync.h       # serverTime drift tracking
│   ├── api.h             # free fns templated on Net::Byte_Stream:
│   │                     #   fetch_server_time / fetch_ticker_price
│   │                     #   fetch_account / place_order
│   ├── client.h          # Client facade owning a Tls_Mbedtls_Stream
│   ├── async_client.h    # Thread::Future-returning wrappers over Client
│   ├── fanout.h          # Concurrent_Rate_Limiter + dispatch_serial/concurrent
│   ├── order_book.h      # L2 book + Binance depth diff merge protocol
│   ├── ws_client.h       # WS handshake + ping/pong + frame defrag
│   ├── market_stream.h   # /ws subscribe/unsubscribe state
│   └── user_stream.h     # listenKey REST lifecycle + /ws/<key> session
├── examples/
│   └── ticker_dump.cpp   # live demo: connect + sync time + print ticker
└── tests/                # all driven via Memory_Stream
    ├── models.cpp        # JSON round-trip for every typed model
    ├── rate_limiter.cpp  # bucket arithmetic + 429/418 cooldown
    ├── time_sync.cpp     # skew math with stub clock
    ├── api.cpp           # full request/response cycle via Memory_Stream
    ├── order_book.cpp    # snapshot + sequence-number protocol
    ├── ws_client.cpp     # handshake errors + ping/pong + fragmented frames
    ├── fanout.cpp        # concurrent limiter + serial/parallel dispatch
    └── async_client.cpp  # Thread::Future dispatch shape
```

## Build

```sh
# Core + app tests (no TLS, fully hermetic)
make test                  # → 51 tests pass

# Same + TLS adapter + loopback handshake test
sudo apt install -y libmbedtls-dev
SPP_TLS=1 make test        # → 52 tests pass

# Live demo binary
SPP_TLS=1 make ticker-dump
BINANCE_HOST=testnet.binance.vision \
    ./build/bin/app/examples/ticker_dump BTCUSDT
```

## Live verification (testnet)

```text
[info] connected to testnet.binance.vision
[info] serverTime skew = 21 ms, RTT = 503 ms
[info] BTCUSDT price = 73529.99000000
[info] rate-limit used in 1m window = 3
```

That's a real TLS handshake → time sync → typed ticker fetch against
Binance's public testnet endpoint, with the rate limiter correctly
accounting for the 2-weight ticker call after the 1-weight time call.

## Production checklist

1. **Install mbedTLS dev headers** (`libmbedtls-dev` on Debian/Ubuntu) and
   build with `SPP_TLS=1`.
2. **Hostname** — keep `config.host = "api.binance.com"` (or
   `"api1-4.binance.com"`); `insecure_skip_verify` must stay false.
3. **API key/secret** — load from env / vault, never literals.
4. **NTP-sync the host** — `Time_Sync` corrects ~hundreds of ms of drift
   but cannot mask a clock that's minutes off. Run `chrony` / `systemd-timesyncd`.
5. **Call `sync_time_if_stale()` before each signed request** (the Client
   facade already does this automatically).
6. **Inspect `rate-limit used` after every fetch** — if a single thread is
   ever near the limit, multi-thread fan-out needs application-level
   throttling.
7. **Handle 429/418 returns** — the limiter installs `cooldown_until_ms`;
   your trading logic must respect it or you'll get the IP banned faster.
8. **Test on `testnet.binance.vision` first** — same TLS, same signing,
   different funds.

## Additional features (this commit)

### Order book maintenance — `binance/order_book.h`

Reads a `Depth_Snapshot` (REST `/api/v3/depth`) plus a stream of
`Depth_Update` events and maintains a sorted L2 book. Implements Binance's
canonical sequence-number protocol exactly:

- `u <= lastUpdateId` → drop (already covered by snapshot)
- `U > lastUpdateId + 1` → gap, caller must resubscribe + resnapshot
- otherwise merge: `qty == 0` removes the level, else replaces it

```cpp
auto book = Bnc::Order_Book::from_snapshot(snapshot);
auto status = book.apply(diff);
if(status == Bnc::Order_Book::Merge_Status::gap) {
    // resnapshot path
}
```

### TLS session resumption — `spp/ext/tls_mbedtls.h`

After a successful handshake, `save_session(buf)` captures the negotiated
session as opaque bytes. A fresh stream's `load_session(slice)` installs it
before its next `connect_result()` so the handshake short-circuits (~1 RTT
+ crypto saved per reconnect). `session_was_resumed()` reports the result.

### Async path — `binance/async_client.h`

`Async_Client` wraps each blocking `Client` method in `Thread::spawn`,
returning a `Thread::Future`. The transport layer is still blocking; this
is enough for "fan out N requests, await all" patterns. Real non-blocking
TLS belongs to a future commit.

### Multi-symbol fan-out — `binance/fanout.h`

`Concurrent_Rate_Limiter` mutex-wraps the bare `Rate_Limiter` so the same
1200-weight-per-minute budget is enforced across threads. `dispatch_serial`
and `dispatch_concurrent` apply a per-item function over a slice, the
latter spawning futures so independent symbol fetches overlap.

### WebSocket sessions — `binance/{ws_client,market_stream,user_stream}.h`

- `Ws_Client<S>` does the RFC 6455 client handshake (with
  `Sec-WebSocket-Accept` verification), masks every outbound frame,
  auto-replies to server PINGs with PONGs, and reassembles fragmented
  data frames into one message.
- `Market_Stream` adds `subscribe(["btcusdt@depth"])` / `unsubscribe`
  on top of `Ws_Client`.
- `User_Stream` pairs with the REST listenKey lifecycle helpers
  (`create_listen_key` / `keepalive_listen_key` / `close_listen_key`)
  for account-specific event streams.

## Architecture choice notes

- **Why free functions + facade, not just a Client class?** Tests inject a
  `Memory_Stream` for hermetic unit tests; production code uses the
  `Tls_Mbedtls_Stream`-backed Client. The free functions are the shared
  business logic so both paths exercise identical code.
- **Why timestamps as `i64` and prices as `String`?** Binance returns
  decimal prices as JSON strings to preserve precision; converting to f64
  on parse would round-trip through float and risk subtle precision loss
  on round-numbered prices. Pass them to `spp::Decimal<>` when arithmetic
  is needed.
- **Why a callable clock parameter on `refresh_with_clock`?** Otherwise
  unit tests can't pin the local clock, which makes skew math
  non-deterministic. Production users call plain `refresh()`, which uses
  the real wall clock.
