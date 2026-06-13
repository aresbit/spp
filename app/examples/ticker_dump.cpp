// Minimal Binance Spot smoke-test executable. Connects to Binance Spot REST
// over TLS, syncs the local clock against serverTime, and prints the live
// price for a single symbol.
//
// Build (from the repo root):
//   SPP_TLS=1 make app/build/ticker_dump
// Run:
//   ./app/build/ticker_dump BTCUSDT
//
// Defaults to the testnet host so this is safe to invoke without real keys.
// Set BINANCE_HOST=api.binance.com to hit production once you've verified.
//
// Requires:
//   - libmbedtls-dev installed
//   - outbound 443 to *.binance.com (or testnet.binance.vision)
//   - NTP-synced system clock (recvWindow rejects > 5s drift)

#include <spp/core/base.h>
#include <spp/reflection/log.h>

#include <binance/api.h>
#include <binance/client.h>

#include <stdlib.h>

namespace Bnc = spp::App::Binance;

spp::i32 main(spp::i32 argc, char** argv) {
    using namespace spp;

    String_View symbol = "BTCUSDT"_v;
    if(argc >= 2) symbol = String_View{argv[1]};

    const char* host_env = ::getenv("BINANCE_HOST");
    String_View host = host_env ? String_View{host_env} : "testnet.binance.vision"_v;

    Bnc::Client_Config cfg;
    cfg.host = host;
    cfg.port = 443;

    Bnc::Client client{cfg};
    auto connected = client.connect();
    if(!connected.ok()) {
        warn("connect failed: %", connected.unwrap_err());
        return 1;
    }
    info("connected to %", host);

    auto synced = client.sync_time_if_stale();
    if(!synced.ok()) {
        warn("time sync failed: %", synced.unwrap_err());
        return 2;
    }
    info("serverTime skew = % ms, RTT = % ms", client.time.skew_ms, client.time.rtt_ms);

    auto t = client.ticker_price(symbol);
    if(!t.ok()) {
        warn("ticker fetch failed: %", t.unwrap_err());
        return 3;
    }
    info("% price = %", t.unwrap().symbol, t.unwrap().price);
    info("rate-limit used in 1m window = %", client.limiter.buckets[0].used);
    return 0;
}
