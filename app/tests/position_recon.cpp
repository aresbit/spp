#include "test.h"

#include <binance/position_recon.h>
#include <spp/quant/strategy/account.h>

namespace Bnc = spp::App::Binance;
namespace QS  = spp::quant::strategy;

static f64 dec_to_f64(spp::Decimal<8> d) noexcept {
    return static_cast<f64>(d.raw()) /
           static_cast<f64>(spp::Decimal<8>::factor());
}

i32 main() {
    Test test{"empty"_v};

    Trace("executionReport FILLED creates a trade and reconciles fee") {
        // Account starts with 10_000 USDT cash, no position.
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        Bnc::Position_Reconciler<Mdefault> recon{acc};

        // Simulate a BUY of 0.001 BTC @ 40_000 USDT. Notional = 40, real
        // fee = 0.04 (0.1% — Binance Spot defaults can be 0.1% pre-BNB
        // discount). Account's synthetic fee = 40 * 0.0003 = 0.012.
        spp::String_View body =
            R"({"e":"executionReport","s":"BTCUSDT","S":"BUY","x":"TRADE",)"
            R"("X":"FILLED","l":"0.001","L":"40000","n":"0.04","T":1700000000000})"_v;
        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(r.unwrap() == 1);
        assert(recon.fills_applied == 1);

        // Account gained a trade and a long position.
        assert(acc.trades.length() == 1);
        assert(acc.positions.length() == 1);
        assert(acc.positions[0].code == "BTCUSDT"_v);
        assert(acc.positions[0].volume_long > 0.0);
        // dec_to_f64 just confirms price preserved through Decimal<8>.
        assert(dec_to_f64(acc.trades[0].price) == 40000.0);

        // Cash reconciled: started at 10000, the synthetic fee path moved
        // some cash and the recon reapplied the real fee delta. We just
        // sanity-check it's been debited (under 10000, but close).
        assert(acc.balance < 10000.0);
        assert(acc.balance > 9000.0);
    }

    Trace("executionReport with non-TRADE exec_type is ignored") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        Bnc::Position_Reconciler<Mdefault> recon{acc};

        // NEW (order acknowledged but no fill).
        spp::String_View body =
            R"({"e":"executionReport","s":"BTCUSDT","S":"BUY","x":"NEW",)"
            R"("X":"NEW","l":"0","L":"0","T":1700000000000})"_v;
        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(r.unwrap() == 0);
        assert(recon.fills_applied == 0);
        assert(acc.trades.length() == 0);
    }

    Trace("outboundAccountPosition overwrites local USDT balance") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        // Pre-perturb local state to test reconciliation.
        acc.available = 9999.99;
        acc.frozen    = 0.0;
        acc.balance   = 9999.99;

        Bnc::Position_Reconciler<Mdefault> recon{acc, "USDT"_v};

        // Server says: 9500 free + 500 locked.
        spp::String_View body =
            R"({"e":"outboundAccountPosition","u":1700000000000,)"
            R"("B":[{"a":"BTC","f":"0.001","l":"0"},)"
            R"({"a":"USDT","f":"9500.00","l":"500.00"}]})"_v;
        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(r.unwrap() == 1);
        assert(recon.position_snaps_applied == 1);
        assert(acc.available == 9500.0);
        assert(acc.frozen    == 500.0);
        assert(acc.balance   == 10000.0);
    }

    Trace("Unknown event types are counted, not errored") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        Bnc::Position_Reconciler<Mdefault> recon{acc};

        spp::String_View body =
            R"({"e":"listStatus","s":"BTCUSDT","g":1})"_v;
        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(recon.events_unknown == 1);
    }

    Trace("No account attached → graceful error") {
        Bnc::Position_Reconciler<Mdefault> recon;
        auto r = recon.apply_event(R"({"e":"executionReport"})"_v);
        assert(!r.ok());
        assert(r.unwrap_err() == "recon_no_account"_v);
    }

    return 0;
}
