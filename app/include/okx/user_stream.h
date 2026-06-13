#pragma once

// OKX V5 private WebSocket: login + subscribe to user-data channels.
//
// Unlike Binance (which mints a `listenKey` over REST then connects to a
// per-key WS URL), OKX uses ONE private WS endpoint for everyone and
// authenticates by emitting a `login` op on the open socket:
//
//   {"op":"login","args":[{
//      "apiKey":"…",
//      "passphrase":"…",
//      "timestamp":"1700000000",            ← UNIX seconds, NOT ISO 8601
//      "sign":"base64(HMAC-SHA256(secret, ts + 'GET' + '/users/self/verify'))"
//   }]}
//
// The server replies with `{"event":"login","code":"0",…}` on success.
// Once logged in, the same socket can subscribe to channels like
// `orders`, `account`, `positions`.
//
// `Position_Reconciler_Okx` mirrors the Binance reconciler: it digests
// `orders` channel TRADE events into `Account::receive_simpledeal` and
// `account` events into `Account::available / frozen / balance`, so the
// strategy's local view matches what OKX actually filled.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/quant/data/types.h>
#include <spp/quant/strategy/account.h>
#include <spp/quant/strategy/types.h>

#include <binance/bar_aggregator.h>   // detail::json_field_ + parse helpers
#include <binance/ws_client.h>        // protocol-level Ws_Client

#include <okx/clock.h>
#include <okx/market_stream.h>        // k_public_ws_host / port / paths
#include <okx/signer.h>

namespace spp::App::Okx {

template<typename S>
struct User_Stream {
    Binance::Ws_Client<S> ws;
    // Active subscriptions — Ws_Session re-issues these after a
    // reconnect.  Login credentials (api_key/secret/passphrase) are
    // borrowed by the Signer_Config the caller hands to `login`; we
    // re-borrow on resubscribe.
    Vec<Owned_Subscription, Mdefault> active_subs;
    Signer_Config last_login_cfg;
    bool logged_in = false;

    explicit User_Stream(S& s) noexcept : ws(s) {
    }

    [[nodiscard]] Result<u64, String_View>
    open(String_View host = k_public_ws_host,
         String_View path = k_private_ws_path) noexcept {
        return ws.handshake(host, path);
    }

    // Send the login op. Returns ok after WRITING the login bytes — the
    // caller is expected to drain `recv_event` until they see the
    // `{"event":"login","code":"0"}` ack.  The credentials are cached
    // so `Ws_Session` can re-login after a reconnect; the caller must
    // keep the underlying String_View storage alive for as long as the
    // User_Stream is in use.
    [[nodiscard]] Result<u64, String_View>
    login(const Signer_Config& cfg, i64 now_ms_value) noexcept {
        last_login_cfg = cfg;
        auto ts = unix_sec_str(now_ms_value);
        // login signs the FIXED path `/users/self/verify` with method
        // GET and no body — same HMAC primitive as REST, different inputs.
        auto sig = sign<Mdefault>(ts.view(), "GET"_v,
                                  "/users/self/verify"_v, ""_v,
                                  cfg.api_secret);

        Vec<u8, Mdefault> buf;
        auto push_lit = [&buf](const char* s) {
            while(*s) buf.push(static_cast<u8>(*s++));
        };
        auto push_sv = [&buf](String_View sv) {
            for(u64 i = 0; i < sv.length(); i++) buf.push(sv[i]);
        };
        push_lit("{\"op\":\"login\",\"args\":[{\"apiKey\":\"");
        push_sv(cfg.api_key);
        push_lit("\",\"passphrase\":\"");
        push_sv(cfg.passphrase);
        push_lit("\",\"timestamp\":\"");
        push_sv(ts.view());
        push_lit("\",\"sign\":\"");
        push_sv(sig.view());
        push_lit("\"}]}");
        auto r = ws.send_text(String_View{buf.data(), buf.length()});
        if(r.ok()) logged_in = true;
        return r;
    }

    [[nodiscard]] Result<u64, String_View>
    subscribe(Slice<const Subscription> subs) noexcept {
        auto r = send_subscribe_(subs);
        if(r.ok()) {
            for(u64 i = 0; i < subs.length(); i++) {
                Owned_Subscription os;
                os.channel = subs[i].channel.template string<Mdefault>();
                os.instId  = subs[i].instId.template string<Mdefault>();
                active_subs.push(spp::move(os));
            }
        }
        return r;
    }

    // After a reconnect: re-login first, then re-subscribe to every
    // channel we had open.  The Ws_Session reconnect callback calls
    // this; it's safe to call standalone if the caller has cached
    // credentials.
    [[nodiscard]] Result<u64, String_View>
    relogin_and_resubscribe(i64 now_ms_value) noexcept {
        if(last_login_cfg.api_key.length() == 0) {
            // Never logged in before — nothing to replay.
            return Result<u64, String_View>::ok(0);
        }
        logged_in = false;
        auto l = login(last_login_cfg, now_ms_value);
        if(!l.ok()) return l;
        if(active_subs.length() == 0) return Result<u64, String_View>::ok(0);
        Vec<Subscription, Mdefault> view_subs;
        for(u64 i = 0; i < active_subs.length(); i++) {
            view_subs.push(Subscription{active_subs[i].channel.view(),
                                         active_subs[i].instId.view()});
        }
        return send_subscribe_(view_subs.slice());
    }

private:
    [[nodiscard]] Result<u64, String_View>
    send_subscribe_(Slice<const Subscription> subs) noexcept {
        Vec<u8, Mdefault> buf;
        auto push_lit = [&buf](const char* s) {
            while(*s) buf.push(static_cast<u8>(*s++));
        };
        auto push_sv = [&buf](String_View sv) {
            for(u64 i = 0; i < sv.length(); i++) buf.push(sv[i]);
        };
        push_lit("{\"op\":\"subscribe\",\"args\":[");
        for(u64 i = 0; i < subs.length(); i++) {
            if(i > 0) buf.push(',');
            push_lit("{\"channel\":\"");
            push_sv(subs[i].channel);
            if(subs[i].instId.length() > 0) {
                push_lit("\",\"instId\":\"");
                push_sv(subs[i].instId);
            }
            push_lit("\"}");
        }
        push_lit("]}");
        return ws.send_text(String_View{buf.data(), buf.length()});
    }

public:

    [[nodiscard]] Result<Vec<u8, Mdefault>, String_View> recv_event() noexcept {
        return ws.recv_message();
    }
};

// Position reconciler for OKX user-data events.
//
// OKX's `orders` channel emits one event per state change. The fields
// we care about for fills:
//
//   {"arg":{"channel":"orders","instType":"SPOT"},
//    "data":[{
//      "instId":"BTC-USDT",
//      "ordId":"312269865356374016",
//      "clOrdId":"local_id_abc",
//      "side":"buy",
//      "fillPx":"42000.0",
//      "fillSz":"0.001",
//      "fillFee":"-0.012",          ← negative = paid
//      "fillFeeCcy":"USDT",
//      "state":"filled" | "partially_filled" | …
//    }]}
//
// `account` channel emits cash balance snapshots — we use them as
// periodic resync (in case a fill event was dropped):
//
//   {"arg":{"channel":"account"},
//    "data":[{
//      "uTime":"…",
//      "totalEq":"…",
//      "details":[{"ccy":"USDT","cashBal":"…","availBal":"…",
//                  "frozenBal":"…", …}]
//    }]}
template<typename A = Mdefault>
struct Position_Reconciler_Okx {
    spp::quant::strategy::Account<A>* account = null;
    String_View quote_asset = "USDT"_v;

    u64 fills_applied = 0;
    u64 position_snaps_applied = 0;
    u64 events_unknown = 0;

    Position_Reconciler_Okx() noexcept = default;
    explicit Position_Reconciler_Okx(spp::quant::strategy::Account<A>& acc,
                                      String_View quote = "USDT"_v) noexcept
        : account(&acc), quote_asset(quote) {
    }

    // Apply one WS frame. Dispatch on the embedded `arg.channel`.
    [[nodiscard]] Result<u64, String_View> apply_event(String_View body) noexcept {
        if(account == null) {
            return Result<u64, String_View>::err("recon_no_account"_v);
        }
        // Quick reject for login/subscribe acks (they have `event`, no `arg`).
        auto evt = Binance::detail::json_field_(body, "event"_v);
        if(evt.length() > 0) {
            // Login / subscribe ack / error — accounted but no fill to apply.
            events_unknown++;
            return Result<u64, String_View>::ok(0);
        }

        auto channel = find_channel_(body);
        if(channel == "orders"_v) return apply_orders_(body);
        if(channel == "account"_v) return apply_account_(body);
        events_unknown++;
        return Result<u64, String_View>::ok(0);
    }

private:
    // `arg`:{`channel`:"…"} — fish out the channel string. We can't reuse
    // the flat json_field_ for "channel" alone since "channel" appears
    // both inside `arg` (which we want) and possibly inside data entries.
    // Looking for it just after "arg" is the safe path.
    [[nodiscard]] static String_View find_channel_(String_View body) noexcept {
        for(u64 i = 0; i + 7 < body.length(); i++) {
            if(body[i] == '"' && body[i + 1] == 'a' && body[i + 2] == 'r' &&
               body[i + 3] == 'g' && body[i + 4] == '"') {
                // Past `"arg"`, find the matching `{`.
                u64 j = i + 5;
                while(j < body.length() && body[j] != '{') j++;
                if(j >= body.length()) break;
                // Track depth so we don't escape `arg` into `data`.
                u64 start = j + 1;
                i32 depth = 1;
                u64 end = start;
                while(end < body.length() && depth > 0) {
                    if(body[end] == '{') depth++;
                    else if(body[end] == '}') depth--;
                    if(depth > 0) end++;
                }
                String_View arg{body.data() + start, end - start};
                return Binance::detail::json_field_(arg, "channel"_v);
            }
        }
        return ""_v;
    }

    [[nodiscard]] Result<u64, String_View> apply_orders_(String_View body) noexcept {
        // Walk each order entry in `data`. The Okx::next_trade helper
        // does the right "find data:[, walk objects" thing — reuse it
        // for the cursor advancement, then re-extract per-entry fields
        // below from the bytes we just walked.
        u64 fills = 0;
        u64 c = 0;
        while(true) {
            Okx_Trade scratch;
            if(!find_next_object_in_data_(body, c, scratch)) break;

            // `scratch` populates the trades-channel fields. For orders
            // channel we need state / fillPx / fillSz / fillFee — go
            // back to the raw bytes and pull them out.
            String_View entry = last_object_view_(body, scratch.instId, c);
            auto state   = Binance::detail::json_field_(entry, "state"_v);
            if(state != "filled"_v && state != "partially_filled"_v) {
                continue;
            }
            auto instId  = Binance::detail::json_field_(entry, "instId"_v);
            auto side    = Binance::detail::json_field_(entry, "side"_v);
            auto fillPx  = Binance::detail::json_field_(entry, "fillPx"_v);
            auto fillSz  = Binance::detail::json_field_(entry, "fillSz"_v);
            auto fillFee = Binance::detail::json_field_(entry, "fillFee"_v);
            if(instId.length() == 0 || fillPx.length() == 0 ||
               fillSz.length() == 0) continue;

            f64 qty   = Binance::detail::parse_decimal_(fillSz);
            f64 price = Binance::detail::parse_decimal_(fillPx);
            f64 fee_real = fillFee.length() > 0
                ? -Binance::detail::parse_decimal_(fillFee) // OKX reports negative for paid fees
                : 0.0;
            if(qty <= 0.0 || price <= 0.0) continue;

            using Dir    = spp::quant::strategy::Order_Direction;
            using Offset = spp::quant::strategy::Order_Offset;
            Dir dir = side == "sell"_v ? Dir::sell : Dir::buy;
            Offset off = side == "sell"_v ? Offset::close_ : Offset::open_;

            Decimal<8> dec_px = spp::quant::data::f64_to_price(price);
            auto trade = account->receive_simpledeal(instId, dec_px, qty, dir, off);
            (void)trade;

            // Reconcile the actual fee: receive_simpledeal applied a
            // synthetic 0.03% (Account::make_deal hardcodes that).
            // Subtract the delta so the local cash converges to OKX's.
            f64 notional = qty * price;
            f64 fee_synth = notional * 0.0003;
            f64 delta = fee_real - fee_synth;
            if(delta != 0.0) {
                account->available -= delta;
                account->balance   -= delta;
            }
            fills_applied++;
            fills++;
        }
        return Result<u64, String_View>::ok(spp::move(fills));
    }

    [[nodiscard]] Result<u64, String_View>
    apply_account_(String_View body) noexcept {
        // The shape is `data:[{… "details":[{ccy, availBal, frozenBal, …}]}]`.
        // Find `details:[` then scan for our quote-asset entry.
        u64 i = 0;
        bool found_details = false;
        while(i + 9 < body.length()) {
            if(body[i] == '"' && body[i + 1] == 'd' && body[i + 2] == 'e' &&
               body[i + 3] == 't' && body[i + 4] == 'a' && body[i + 5] == 'i' &&
               body[i + 6] == 'l' && body[i + 7] == 's' && body[i + 8] == '"') {
                u64 j = i + 9;
                while(j < body.length() &&
                      (body[j] == ' ' || body[j] == ':' ||
                       body[j] == '\t' || body[j] == '\n')) {
                    j++;
                }
                if(j < body.length() && body[j] == '[') {
                    i = j + 1;
                    found_details = true;
                    break;
                }
            }
            i++;
        }
        if(!found_details) return Result<u64, String_View>::ok(0);

        while(i < body.length() && body[i] != ']') {
            while(i < body.length() && body[i] != '{' && body[i] != ']') i++;
            if(i >= body.length() || body[i] == ']') break;
            u64 start = i;
            i32 depth = 1;
            i++;
            while(i < body.length() && depth > 0) {
                if(body[i] == '{') depth++;
                else if(body[i] == '}') depth--;
                if(depth > 0) i++;
            }
            if(i >= body.length()) break;
            i++; // past closing '}'
            String_View entry{body.data() + start, i - start};
            auto ccy = Binance::detail::json_field_(entry, "ccy"_v);
            if(ccy == quote_asset) {
                auto availBal  = Binance::detail::json_field_(entry, "availBal"_v);
                auto frozenBal = Binance::detail::json_field_(entry, "frozenBal"_v);
                f64 avail_v = Binance::detail::parse_decimal_(availBal);
                f64 frzn_v  = Binance::detail::parse_decimal_(frozenBal);
                account->available = avail_v;
                account->frozen    = frzn_v;
                account->balance   = avail_v + frzn_v;
                position_snaps_applied++;
                return Result<u64, String_View>::ok(1);
            }
        }
        return Result<u64, String_View>::ok(0);
    }

    // Walk `data:[…]` and return true with cursor advanced past one
    // object. Side-effects out_ with whatever next_trade would extract
    // (we ignore it; just need cursor advancement).
    static bool find_next_object_in_data_(String_View body, u64& cursor,
                                           Okx_Trade& out_) noexcept {
        return next_trade(body, cursor, out_);
    }

    // The walker advances `cursor` past the closing `}`. Recover the
    // entry's bytes by going back: search backwards from cursor-1 for
    // the matching `{`. We use the `instId` value we just extracted as
    // a sentinel only to confirm we're in the right entry.
    static String_View last_object_view_(String_View body, String_View,
                                          u64 cursor_after) noexcept {
        // cursor_after points just past `}`. Walk backwards counting
        // braces to find the matching `{`.
        if(cursor_after == 0 || cursor_after > body.length()) return ""_v;
        u64 end = cursor_after;
        i32 depth = 0;
        u64 i = cursor_after;
        while(i > 0) {
            i--;
            if(body[i] == '}') depth++;
            else if(body[i] == '{') {
                depth--;
                if(depth == 0) {
                    return String_View{body.data() + i, end - i};
                }
            }
        }
        return ""_v;
    }
};

} // namespace spp::App::Okx
