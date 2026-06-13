// OKX spot live-trading demo. Build with:
//   SPP_TLS=1 make build/bin/app/examples/okx_demo
// Run with:
//   OKX_API_KEY=... OKX_SECRET=... OKX_PASSPHRASE=... OKX_SIMULATED=1 \
//   ./build/bin/app/examples/okx_demo [symbol] [cash]
// Ctrl-C shuts down cleanly.

#include <binance/filter_round.h>
#include <okx/client.h>
#include <okx/event_loop.h>
#include <okx/live_driver.h>
#include <okx/market_stream.h>
#include <okx/user_stream.h>
#include <strategies/chan_live.h>
#include <spp/io/files.h>
#include <spp/io/wal.h>
#include <signal.h>

using namespace spp;
using namespace spp::App::Okx;
using namespace spp::App::Strategies;

static volatile bool g_run = true;
static void handle_sig(int) noexcept { g_run = false; }

int main(int argc, char** argv) {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    // 1. Credentials from env.
    auto gs = [](const char* n) -> String_View {
        const char* e = getenv(n);
        return e ? String_View((const u8*)e, Libc::strlen(e)) : ""_v;
    };
    auto key = gs("OKX_API_KEY");
    auto sec = gs("OKX_SECRET");
    auto pas = gs("OKX_PASSPHRASE");
    if (key.length() == 0) return 1;
    bool sim = gs("OKX_SIMULATED") == "1"_v;

    // 2. Symbol + cash from CLI.
    String_View sym =
        argc > 1 ? String_View((const u8*)argv[1], Libc::strlen(argv[1])) : "BTC-USDT"_v;
    f64 cash = argc > 2 ? Libc::strtof(argv[2], nullptr) : 100.0;

    // 3. REST client.
    Client_Config cc; cc.api_key=key; cc.api_secret=sec;
    cc.passphrase=pas; cc.simulated_trading=sim;
    Client cli(spp::move(cc));
    if (!cli.connect().ok()) return 1;
    cli.sync_time_now();

    // 4. Filters.
    App::Binance::Filter_Cache<Mdefault> filters;
    { auto ir = cli.instruments();
      if (ir.ok()) populate_filter_cache_from_instruments(filters, ir.unwrap()); }

    // 5. Strategy.
    Chan_Live_Config sc; sc.symbol = sym;
    sc.entry_cash_fraction = 0.20; sc.stop_buffer_pct = 0.02;
    Chan_Live_Strategy strat(spp::move(sc));
    strat.init_cash = cash;
    strat.acc = quant::strategy::Account<Mdefault>{sym.template string<Mdefault>(), cash};
    strat.running_mode = quant::strategy::Running_Mode::live;

    // 6. WAL (crash recovery).
    WAL::Writer wal; wal.open_result("driver.wal"_v, 4096, 4096);

    // 7. Market WS + User WS.
    Ext::Tls_Mbedtls_Stream mt, ut;
    mt.connect_result("ws.okx.com"_v, 8443);
    Market_Stream<Ext::Tls_Mbedtls_Stream> mkt{mt}; mkt.open();
    Subscription sm{"trades"_v, sym};
    mkt.subscribe(Slice<const Subscription>{&sm, 1});

    ut.connect_result("ws.okx.com"_v, 8443);
    User_Stream<Ext::Tls_Mbedtls_Stream> us{ut}; us.open();
    us.login(cli.signer, now_ms());
    Subscription u1{"orders"_v, sym}, u2{"account"_v, ""_v};
    Subscription ua[2] = {u1, u2};
    us.subscribe(Slice<const Subscription>{ua, 2});

    // 8. Driver.
    Live_Driver<Chan_Live_Strategy, Ext::Tls_Mbedtls_Stream,
                 Ext::Tls_Mbedtls_Stream> drv{mkt, cli.stream(), cli.limiter, strat};
    drv.signer       = cli.signer;
    drv.filter_cache = &filters;
    drv.wal_writer   = &wal;
    Position_Reconciler_Okx<Mdefault> recon{strat.acc, "USDT"_v};
    drv.attach_reconciler(recon);
    drv.register_instrument(sym, 60000);

    // 9. Risk gates.
    quant::risk::Risk_Config rc;
    rc.max_single_order_value = cash * 0.10;
    rc.max_daily_trades = 20;
    rc.daily_loss_limit_pct = 0.20;
    rc.max_position_weight = 0.25;
    quant::risk::Risk_Checker<Mdefault> rcheck(rc);
    drv.risk_checker = &rcheck;
    rcheck.state.daily_start_equity = cash;

    // 10. Event loop.
    u64 last_data_ms = now_ms();
    while (g_run) {
        i64 n = now_ms();

        // Kill switch.
        { auto ks = Files::read_result("/tmp/spp_kill"_v);
          if (ks.ok()) break; }

        // Stale-data warning (>5min no trades).
        if (n - (i64)last_data_ms > 5 * 60 * 1000) {
            // Pong is still flowing, but trades stopped — OKX may have
            // dropped the subscription silently.  Recover by forcing a
            // WS reconnect.
            mkt.ws.reset(); mt.close();
            mt.connect_result("ws.okx.com"_v, 8443);
            mkt.open(); mkt.resubscribe_all();
            last_data_ms = n;
        }

        auto r = poll_either(mt, ut, 500);
        if (!r.ok()) { Thread::sleep(100); continue; }
        if (ready_market(r.unwrap())) {
            auto mr = drv.pump_market_once(n);
            if (mr.ok()) last_data_ms = n;
        }
        if (ready_user(r.unwrap())) static_cast<void>(drv.pump_user_once(us));

        // Keep the risk circuit-breakers LIVE.  Risk_Checker's daily-loss
        // and drawdown gates act on `state` that only record_trade /
        // update_drawdown mutate — and nothing on the fill path calls
        // them.  Without this refresh, daily_loss_limit_pct and
        // max_drawdown_pct are configured but never trip.  We drive them
        // off account equity (realized via balance + unrealized PnL); once
        // breached, check_order blocks new entries while existing fills
        // keep reconciling.
        {
            f64 eq = strat.acc.total_equity();
            rcheck.state.daily_pnl = eq - rcheck.state.daily_start_equity;
            rcheck.update_drawdown(eq);  // updates peak + halts on drawdown
        }
    }

    // Shutdown.
    for (u64 i = 0; i < strat.acc.orders.length(); i++)
        if (strat.acc.orders[i].status == 0) strat.acc.orders[i].status = 2;
    drv.dispatch_cancellations_(now_ms());
    Files::remove_result("/tmp/spp_kill"_v);
    return 0;
}
