#pragma once

// OKX V5 public WebSocket market stream.
//
// OKX's subscription protocol differs from Binance in two ways:
//
//   1. Subscribe messages are
//        {"op":"subscribe","args":[{"channel":"<ch>","instId":"<sym>"}]}
//      not Binance's
//        {"method":"SUBSCRIBE","params":["btcusdt@trade"],"id":N}
//
//   2. Trade events arrive wrapped:
//        {"arg":{"channel":"trades","instId":"BTC-USDT"},
//         "data":[{"instId":"BTC-USDT","tradeId":"…","px":"42000.0",
//                  "sz":"0.01","side":"buy","ts":"1700000000123"}]}
//      whereas Binance sends a flat aggTrade per frame.
//
// We reuse the protocol-level Ws_Client + Bar_Aggregator we built for
// Binance — only the JSON shape changes.  `Okx_Trade_Adapter` extracts
// the fields needed to drive a `Bar_Aggregator<...>` from an OKX trade
// frame.

#include <spp/core/base.h>
#include <spp/core/result.h>

#include <binance/bar_aggregator.h>   // Bar_Aggregator + detail::json_field_
#include <binance/ws_client.h>        // protocol-level Ws_Client

namespace spp::App::Okx {

inline const String_View k_public_ws_host = "ws.okx.com"_v;
constexpr u16 k_public_ws_port = 8443;
inline const String_View k_public_ws_path = "/ws/v5/public"_v;
inline const String_View k_private_ws_path = "/ws/v5/private"_v;

// Borrow the Binance WS client wholesale — it speaks RFC 6455, not
// Binance-specific bytes.
template<typename S>
using Ws_Client = App::Binance::Ws_Client<S>;

// A single (channel, instId) subscription target.
struct Subscription {
    String_View channel;   // e.g. "trades", "candle1m", "tickers"
    String_View instId;    // e.g. "BTC-USDT"
};

// Persisted Subscription so the session can re-issue it after a
// reconnect.  Wire `Subscription` borrows views into caller-owned
// strings; the persisted form owns them.
struct Owned_Subscription {
    String<Mdefault> channel;
    String<Mdefault> instId;
};

template<typename S>
    requires Net::Byte_Stream<S>
struct Market_Stream {
    Ws_Client<S> ws;
    // Active subscriptions — Ws_Session reads this when re-opening
    // after a reconnect to re-issue every channel.
    Vec<Owned_Subscription, Mdefault> active_subs;

    explicit Market_Stream(S& s) noexcept : ws(s) {
    }

    [[nodiscard]] Result<u64, String_View>
    open(String_View host = k_public_ws_host,
         String_View path = k_public_ws_path) noexcept {
        return ws.handshake(host, path);
    }

    [[nodiscard]] Result<u64, String_View>
    subscribe(Slice<const Subscription> subs) noexcept {
        auto r = send_admin_("subscribe"_v, subs);
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

    [[nodiscard]] Result<u64, String_View>
    unsubscribe(Slice<const Subscription> subs) noexcept {
        auto r = send_admin_("unsubscribe"_v, subs);
        // Best-effort cleanup of `active_subs` on success — drop any
        // entry whose (channel, instId) matches one of the unsubscribed
        // tuples.  Matters only for re-issue correctness on reconnect.
        if(r.ok()) {
            for(u64 i = 0; i < subs.length(); i++) {
                for(u64 j = 0; j < active_subs.length(); /*manual*/) {
                    if(active_subs[j].channel == subs[i].channel &&
                       active_subs[j].instId  == subs[i].instId) {
                        // Swap-with-last + pop to keep O(1) removal.
                        active_subs[j] = spp::move(
                            active_subs[active_subs.length() - 1]);
                        active_subs.pop();
                    } else {
                        j++;
                    }
                }
            }
        }
        return r;
    }

    // Re-issue every cached subscription as a single subscribe op.
    // Called by `Ws_Session` right after a successful reconnect.
    [[nodiscard]] Result<u64, String_View> resubscribe_all() noexcept {
        if(active_subs.length() == 0) return Result<u64, String_View>::ok(0);
        Vec<Subscription, Mdefault> view_subs;
        for(u64 i = 0; i < active_subs.length(); i++) {
            view_subs.push(Subscription{active_subs[i].channel.view(),
                                         active_subs[i].instId.view()});
        }
        // send_admin_ doesn't write to active_subs so the bookkeeping
        // stays consistent.
        return send_admin_("subscribe"_v, view_subs.slice());
    }

    [[nodiscard]] Result<Vec<u8, Mdefault>, String_View> recv() noexcept {
        return ws.recv_message();
    }

private:
    [[nodiscard]] Result<u64, String_View>
    send_admin_(String_View op, Slice<const Subscription> subs) noexcept {
        Vec<u8, Mdefault> buf;
        auto push_lit = [&buf](const char* s) {
            while(*s) buf.push(static_cast<u8>(*s++));
        };
        auto push_sv = [&buf](String_View sv) {
            for(u64 i = 0; i < sv.length(); i++) buf.push(sv[i]);
        };

        push_lit("{\"op\":\"");
        push_sv(op);
        push_lit("\",\"args\":[");
        for(u64 i = 0; i < subs.length(); i++) {
            if(i > 0) buf.push(',');
            push_lit("{\"channel\":\"");
            push_sv(subs[i].channel);
            // Channels like "account" don't take instId — emitting an
            // empty value makes OKX reject the whole subscribe op.
            if(subs[i].instId.length() > 0) {
                push_lit("\",\"instId\":\"");
                push_sv(subs[i].instId);
            }
            push_lit("\"}");
        }
        push_lit("]}");
        return ws.send_text(String_View{buf.data(), buf.length()});
    }
};

// Extract the first trade entry from an OKX `trades` channel frame.
// Returns four fields suitable for `Bar_Aggregator::on_message`-style
// flow: instId, px, sz, ts (all as String_View into the input).
//
// OKX may batch multiple trades in one frame (`data` is an array of
// {tradeId, px, sz, ts, ...}); the caller is expected to loop over them
// — `next_trade` advances `cursor` past the trade just read.
struct Okx_Trade {
    String_View instId;
    String_View px;
    String_View sz;
    String_View side;       // "buy" or "sell"
    String_View ts;
};

// Find the next `{"instId":"…","tradeId":...}` object inside the
// outer message's `data:[…]` array, populate `out`, and advance
// `cursor` past the closing `}`.  Returns false when no more trades
// remain.  On the first call (cursor == 0) the function seeks the
// `data:[` array opener before walking; subsequent calls assume
// cursor is already inside the array.
[[nodiscard]] inline bool
next_trade(String_View body, u64& cursor, Okx_Trade& out) noexcept {
    if(cursor == 0) {
        // Locate `"data":[` and position cursor just past the `[` so
        // the object walk below works at the correct nesting depth.
        bool found = false;
        for(u64 i = 0; i + 7 < body.length(); i++) {
            if(body[i] == '"' && body[i + 1] == 'd' && body[i + 2] == 'a' &&
               body[i + 3] == 't' && body[i + 4] == 'a' && body[i + 5] == '"') {
                u64 j = i + 6;
                while(j < body.length() &&
                      (body[j] == ' ' || body[j] == ':' ||
                       body[j] == '\t' || body[j] == '\n')) {
                    j++;
                }
                if(j < body.length() && body[j] == '[') {
                    cursor = j + 1;
                    found = true;
                    break;
                }
            }
        }
        if(!found) return false;
    }

    while(cursor < body.length() && body[cursor] != '{') {
        if(body[cursor] == ']') return false;
        cursor++;
    }
    if(cursor >= body.length()) return false;
    u64 start = cursor;
    i32 depth = 0;
    while(cursor < body.length()) {
        if(body[cursor] == '{') depth++;
        else if(body[cursor] == '}') {
            depth--;
            if(depth == 0) { cursor++; break; }
        }
        cursor++;
    }
    if(depth != 0) return false;
    String_View entry{body.data() + start, cursor - start};

    out.instId = App::Binance::detail::json_field_(entry, "instId"_v);
    out.px     = App::Binance::detail::json_field_(entry, "px"_v);
    out.sz     = App::Binance::detail::json_field_(entry, "sz"_v);
    out.side   = App::Binance::detail::json_field_(entry, "side"_v);
    out.ts     = App::Binance::detail::json_field_(entry, "ts"_v);
    return true;
}

// Convenience: parse an OKX trades frame and feed every embedded trade
// to a `Bar_Aggregator` keyed on `instId`.  Returns the number of
// trades dispatched.  Frames with mismatched instId are silently
// skipped (matches Bar_Aggregator's own behaviour).
template<typename Cb>
inline u64 feed_aggregator(String_View body,
                            App::Binance::Bar_Aggregator<Cb>& agg) noexcept {
    u64 cursor = 0;
    u64 fed = 0;
    Okx_Trade t;
    while(next_trade(body, cursor, t)) {
        if(t.instId.length() == 0 || t.px.length() == 0 ||
           t.sz.length() == 0 || t.ts.length() == 0) {
            continue;
        }
        // Build a synthetic aggTrade-shaped JSON so the existing
        // Bar_Aggregator's parser (`json_field_` + parse_decimal_) keeps
        // working unchanged.  Stays inside this function's lifetime so
        // the temporary is safe.
        char buf[256];
        i32 n = Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf),
            "{\"s\":\"%.*s\",\"p\":\"%.*s\",\"q\":\"%.*s\",\"T\":%.*s}",
            (i32)t.instId.length(), t.instId.data(),
            (i32)t.px.length(),     t.px.data(),
            (i32)t.sz.length(),     t.sz.data(),
            (i32)t.ts.length(),     t.ts.data());
        // vsnprintf returns the UNtruncated length; guard against an
        // oversized frame producing n >= sizeof(buf) and an OOB read.
        if(n <= 0 || n >= (i32)sizeof(buf)) continue;
        String_View synth{reinterpret_cast<const u8*>(buf), static_cast<u64>(n)};
        static_cast<void>(agg.on_message(synth));
        fed++;
    }
    return fed;
}

} // namespace spp::App::Okx
