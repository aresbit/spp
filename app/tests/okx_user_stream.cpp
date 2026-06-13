#include "test.h"

#include <spp/io/stream.h>
#include <spp/protocol/websocket.h>
#include <spp/quant/strategy/strategy_base.h>

#include <okx/user_stream.h>

namespace Okx = spp::App::Okx;
namespace Ws  = spp::Protocol::Websocket;
namespace QS  = spp::quant::strategy;

i32 main() {
    Test test{"empty"_v};

    Trace("login op carries apiKey/passphrase/timestamp/sign") {
        spp::Net::Memory_Stream wire;
        Okx::User_Stream<spp::Net::Memory_Stream> us{wire};
        us.ws.handshake_done = true;

        Okx::Signer_Config cfg;
        cfg.api_key    = "test_key"_v;
        cfg.api_secret = "22582BD0CFF14C41EDBF1AB98506286D"_v;
        cfg.passphrase = "test_pass"_v;
        auto r = us.login(cfg, 1700000000000LL);
        assert(r.ok());

        // Decode the masked frame and verify the JSON shape.
        auto sent = wire.sent();
        spp::Vec<u8, Mdefault> mut;
        for(spp::u64 i = 0; i < sent.length(); i++) mut.push(sent[i]);
        auto dec = Ws::decode(mut.slice());
        assert(dec.ok());
        spp::String_View payload{dec.unwrap().frame.payload.data(),
                                 dec.unwrap().frame.payload.length()};

        auto contains = [&](spp::String_View needle) {
            if(needle.length() > payload.length()) return false;
            for(spp::u64 i = 0; i + needle.length() <= payload.length(); i++) {
                if(payload.sub(i, i + needle.length()) == needle) return true;
            }
            return false;
        };
        assert(contains("\"op\":\"login\""_v));
        assert(contains("\"apiKey\":\"test_key\""_v));
        assert(contains("\"passphrase\":\"test_pass\""_v));
        assert(contains("\"timestamp\":\"1700000000\""_v));
        // sign field exists. The exact value depends on the prehash:
        //   "1700000000" + "GET" + "/users/self/verify" + ""
        auto expected_sign = Okx::sign<Mdefault>(
            "1700000000"_v, "GET"_v, "/users/self/verify"_v, ""_v,
            cfg.api_secret);
        auto sig_needle = "\"sign\":\""_v
            .append<Mdefault>(expected_sign.view());
        assert(contains(sig_needle.view()));
    }

    Trace("subscribe op nests channel/instId per arg") {
        spp::Net::Memory_Stream wire;
        Okx::User_Stream<spp::Net::Memory_Stream> us{wire};
        us.ws.handshake_done = true;
        Okx::Subscription subs[2] = {
            {"orders"_v, "BTC-USDT"_v},
            {"account"_v, ""_v},
        };
        auto r = us.subscribe(spp::Slice<const Okx::Subscription>{subs, 2});
        assert(r.ok());

        auto sent = wire.sent();
        spp::Vec<u8, Mdefault> mut;
        for(spp::u64 i = 0; i < sent.length(); i++) mut.push(sent[i]);
        auto dec = Ws::decode(mut.slice());
        assert(dec.ok());
        spp::String_View payload{dec.unwrap().frame.payload.data(),
                                 dec.unwrap().frame.payload.length()};
        // Channels with empty instId must omit the field, not emit "":
        assert(payload ==
            R"({"op":"subscribe","args":[)"
            R"({"channel":"orders","instId":"BTC-USDT"},)"
            R"({"channel":"account"}]})"_v);
    }

    Trace("orders FILLED event creates a Trade in the local account") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        Okx::Position_Reconciler_Okx<Mdefault> recon{acc};

        spp::String_View body =
            R"({"arg":{"channel":"orders","instType":"SPOT"},"data":[)"
            R"({"instId":"BTC-USDT","ordId":"312269865356374016",)"
            R"("clOrdId":"local_abc","side":"buy","fillPx":"42000.0",)"
            R"("fillSz":"0.001","fillFee":"-0.04","fillFeeCcy":"USDT",)"
            R"("state":"filled"}]})"_v;

        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(recon.fills_applied == 1);
        assert(acc.trades.length() == 1);
        // Long position should be opened.
        auto pos = acc.get_position("BTC-USDT"_v);
        assert(pos.ok());
        assert(pos->volume_long > 0.0);
    }

    Trace("orders event with state=live is ignored (no fill)") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        Okx::Position_Reconciler_Okx<Mdefault> recon{acc};

        spp::String_View body =
            R"({"arg":{"channel":"orders","instType":"SPOT"},"data":[)"
            R"({"instId":"BTC-USDT","ordId":"1","side":"buy",)"
            R"("fillPx":"0","fillSz":"0","state":"live"}]})"_v;
        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(recon.fills_applied == 0);
        assert(acc.trades.length() == 0);
    }

    Trace("account snapshot overwrites local USDT balance") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 10000.0};
        // Perturb local state to confirm the reconciler resets it.
        acc.available = 9999.99;
        acc.frozen    = 0.0;
        acc.balance   = 9999.99;

        Okx::Position_Reconciler_Okx<Mdefault> recon{acc, "USDT"_v};

        spp::String_View body =
            R"({"arg":{"channel":"account"},"data":[{"uTime":"1700000000000",)"
            R"("totalEq":"10000.00",)"
            R"("details":[{"ccy":"BTC","availBal":"0.001","frozenBal":"0"},)"
            R"({"ccy":"USDT","availBal":"9500.00","frozenBal":"500.00"}]}]})"_v;
        auto r = recon.apply_event(body);
        assert(r.ok());
        assert(recon.position_snaps_applied == 1);
        assert(acc.available == 9500.0);
        assert(acc.frozen    == 500.0);
        assert(acc.balance   == 10000.0);
    }

    Trace("Login/subscribe acks are counted but never error out") {
        QS::Account<Mdefault> acc{"main"_v.string<Mdefault>(), 1.0};
        Okx::Position_Reconciler_Okx<Mdefault> recon{acc};
        auto r = recon.apply_event(R"({"event":"login","code":"0","msg":""})"_v);
        assert(r.ok());
        assert(recon.events_unknown == 1);
    }

    return 0;
}
