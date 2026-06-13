#pragma once

// OKX live trading driver — counterpart to `Binance::Live_Driver`.
//
// Shape (identical pipeline, OKX wire types):
//
//   OKX public WS trades  ─►  Bar_Aggregator (via feed_aggregator)
//                              │ (on bar close)
//                              ▼
//                     Strategy_Base::x1(Symbol_Bar)
//                              │ (acc.orders accumulates pending entries)
//                              ▼
//                     translate to Okx::Order_Request
//                              │
//                              ▼
//                  Okx::place_order REST  ──►  OKX
//                              │ (on success)
//                              ▼
//              exchange_ord_id_[local_id] = data[0].ordId
//
// The user-data path runs through `pump_user_once(user_stream)` + a
// `Position_Reconciler_Okx` (see `okx/user_stream.h`).
//
// Differences vs the Binance driver:
//
//   - OKX orderId is a STRING (`"312269865356374016"`), not an i64.
//   - place_order takes a `Signer_Config` rather than separate
//     api_key / api_secret + Time_Sync; OKX already stamps the
//     timestamp inside the signer.
//   - Market frames are BATCHED (one frame can carry several trades).
//     We hand the whole frame to `Okx::feed_aggregator` rather than
//     calling `aggregator.on_message` per trade.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/quant/data/types.h>
#include <spp/quant/strategy/types.h>

#include <binance/bar_aggregator.h>
#include <binance/filter_round.h>
#include <spp/quant/risk/risk_checker.h>

#include <okx/api.h>
#include <okx/driver_wal.h>
#include <okx/event_loop.h>
#include <okx/market_stream.h>
#include <okx/rate_limiter.h>
#include <okx/signer.h>
#include <okx/user_stream.h>
#include <okx/ws_session.h>

namespace spp::App::Okx {

namespace detail {

// Decimal<8> → "12345.67890000", trailing zeros trimmed (matches the
// Binance driver's helper).  OKX accepts both trimmed and untrimmed
// strings but the LOT_SIZE filter validation is friendlier with the
// trimmed form.
inline String<Mdefault> decimal_to_string_(Decimal<8> v) noexcept {
    i64 raw = v.raw();
    bool neg = raw < 0;
    u64 abs_raw = neg ? static_cast<u64>(-raw) : static_cast<u64>(raw);
    u64 factor = Decimal<8>::factor();
    u64 whole = abs_raw / factor;
    u64 frac  = abs_raw % factor;

    char buf[40];
    i32 n = 0;
    if(neg) buf[n++] = '-';
    n += Libc::snprintf(reinterpret_cast<u8*>(buf + n), sizeof(buf) - n,
                        "%llu", static_cast<unsigned long long>(whole));
    buf[n++] = '.';
    char frac_buf[16];
    i32 fn = Libc::snprintf(reinterpret_cast<u8*>(frac_buf), sizeof(frac_buf),
                            "%08llu", static_cast<unsigned long long>(frac));
    for(i32 i = 0; i < fn; i++) buf[n++] = frac_buf[i];
    while(n > 0 && buf[n - 1] == '0') n--;
    if(n > 0 && buf[n - 1] == '.') n++;

    String<Mdefault> out(static_cast<u64>(n));
    out.set_length(static_cast<u64>(n));
    Libc::memcpy(out.data(), buf, static_cast<u64>(n));
    return out;
}

// Sanitize a local order id into an OKX-legal clOrdId.  OKX requires
// clOrdId be case-sensitive ALPHANUMERIC ONLY, <=32 chars, and ideally
// begin with a letter — so the account layer's "ORD_<n>" form (which is
// valid on Binance) would be rejected by OKX with a 51000-class error.
// We strip every non-alphanumeric byte ("ORD_123" -> "ORD123"); the map
// is injective for the "<prefix><digits>" id shape so uniqueness holds.
// The result is truncated to 32 bytes to honour OKX's hard limit.
inline String<Mdefault> sanitize_clordid_(String_View id) noexcept {
    char buf[32];
    u64 n = 0;
    for(u64 i = 0; i < id.length() && n < sizeof(buf); i++) {
        u8 c = id.data()[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z');
        if(alnum) buf[n++] = static_cast<char>(c);
    }
    String<Mdefault> out(n);
    out.set_length(n);
    if(n > 0) Libc::memcpy(out.data(), buf, n);
    return out;
}

} // namespace detail

// Configuration for one instId bucket.
struct Inst_Spec {
    String_View instId;    // "BTC-USDT"
    i64 bucket_ms = 60000;
};

// `Market_S`, `Rest_S`, `User_S` are independent Byte_Stream types.
// `Strategy` is the CRTP derived strategy class (Strategy_Base subclass).
template<typename Strategy, typename Market_S, typename Rest_S,
         typename User_S = Market_S, typename A = Mdefault>
    requires Net::Byte_Stream<Market_S> && Net::Byte_Stream<Rest_S> &&
             Net::Byte_Stream<User_S>
struct Live_Driver {
    Market_Stream<Market_S>& market;
    Rest_S& rest;
    Rate_Limiter& limiter;
    Strategy& strategy;
    Position_Reconciler_Okx<A>* reconciler = null;

    Signer_Config signer;
    String_View host = "www.okx.com"_v;
    // td_mode applied to every order this driver dispatches.  Spot
    // default; flip to cross / isolated for margin / perp strategies.
    Td_Mode td_mode = Td_Mode::cash;

    struct Bar_Trampoline {
        Live_Driver* self = null;
        void operator()(const spp::quant::data::Symbol_Bar& sb) const noexcept {
            self->on_bar_(sb);
        }
    };
    Vec<Binance::Bar_Aggregator<Bar_Trampoline>, A> aggregators;

    // Diagnostic counters / cursors.
    u64 bars_closed = 0;
    u64 orders_dispatched = 0;
    u64 dispatch_failures = 0;
    u64 cancels_dispatched = 0;
    u64 cancel_failures = 0;
    u64 dispatch_cursor_ = 0;

    // Local order_id → exchange ordId (string for OKX, unlike Binance's
    // i64).  Populated from each successful place_order response.
    Map<String<A>, String<A>, A> exchange_ord_id_;
    // Idempotency set for cancels.
    Map<String<A>, u8, A> cancel_done_;

    String_View last_dispatch_err_;
    String_View last_cancel_err_;
    // Owned copy of the most recent OKX per-order rejection code (sCode).
    // last_dispatch_err_ is a String_View and the response it came from is
    // freed at end of dispatch; clone the code here so it stays readable.
    String<A> last_dispatch_scode_;

    // Optional symbol-filter cache — see the Binance driver's docstring
    // for semantics.  Leave null to bypass (tests do).
    Binance::Filter_Cache<A>* filter_cache = null;

    // Optional risk gate.  When set, every outgoing order is pre-
    // checked against configured limits (max single order, max daily
    // loss, max drawdown, max exposure).  A rejected order is counted
    // as a dispatch failure and the cursor advances past it.
    quant::risk::Risk_Checker<A>* risk_checker = null;

    // Persistent WAL for crash recovery.  When non-null, every placed
    // order, confirmed exchange-mapping, and cancellation is written
    // to the underlying file before the dispatch loop continues.  On
    // startup, replay_wal() feeds the raw bytes back in.
    WAL::Writer* wal_writer = null;
    // Offset in the WAL of the most recent confirm / cancel entry.
    // Used by flush() to fsync; checkpoint logic can read it.
    u64 wal_last_commit_offset_ = 0;

    // Replay a raw WAL byte buffer from a prior session into the
    // driver's exchange_ord_id_ and cancel_done_ maps.  Use this right
    // after construction, before the main loop starts.
    void replay_wal(Slice<const u8> raw) noexcept {
        auto s = App::Okx::replay_wal<A>(raw);
        exchange_ord_id_ = spp::move(s.exchange_ord_id);
        cancel_done_     = spp::move(s.cancel_done);
        // pending_at_crash is informational — the caller can retrieve
        // unconfirmed orders from strategy.acc.orders between the
        // reconstructed dispatch_cursor_ and orders.length().
    }

    Live_Driver(Market_Stream<Market_S>& m, Rest_S& r, Rate_Limiter& lim,
                Strategy& s) noexcept
        : market(m), rest(r), limiter(lim), strategy(s) {
    }

    void attach_reconciler(Position_Reconciler_Okx<A>& rec) noexcept {
        reconciler = &rec;
    }

    // Register an aggregator for `instId` so the WS pump knows where to
    // route incoming trades. The actual WS `subscribe` op is the
    // caller's responsibility (call `market.subscribe(...)`).
    void register_instrument(String_View instId, i64 bucket_ms = 60000) noexcept {
        aggregators.push(Binance::Bar_Aggregator<Bar_Trampoline>{
            instId, bucket_ms, Bar_Trampoline{this}});
    }

    [[nodiscard]] String_View
    exchange_id_for(String_View local_id) const noexcept {
        auto e = exchange_ord_id_.try_get(local_id);
        return e.ok() ? (**e).view() : ""_v;
    }

    // Drain ONE WS frame from the bare market stream and route the
    // trades inside it. Used by the test harness — production goes
    // through `pump_market_session_once` so reconnects + ping/pong
    // are handled.
    [[nodiscard]] Result<u64, String_View> pump_market_once(i64 now_ms) noexcept {
        auto msg = market.recv();
        if(!msg.ok()) {
            return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
        }
        return route_market_bytes_(msg.unwrap().slice(), now_ms);
    }

    // Session-aware variant: recv_or_reconnect transparently handles
    // TLS+WS reconnect and pong-frame swallowing. The caller's event
    // loop just calls this in a tight loop with `pump_either_session`.
    template<typename Replay, typename Connector>
    [[nodiscard]] Result<u64, String_View>
    pump_market_session_once(
        Ws_Session<Market_Stream<Market_S>, Replay, Connector>& session,
        i64 now_ms) noexcept {
        auto msg = session.recv_or_reconnect(now_ms);
        if(!msg.ok()) {
            return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
        }
        return route_market_bytes_(msg.unwrap().slice(), now_ms);
    }

    // Multiplexed pump for production deployments — waits on BOTH the
    // market and user TLS streams simultaneously and dispatches
    // whichever is ready first (or both, if they fired in the same
    // poll wakeup).  Tests use `pump_market_once` / `pump_user_once`
    // directly with Memory_Stream and don't need this path.
    //
    // `market_tls` and `user_tls` are the bare TLS streams (e.g.
    // `Ext::Tls_Mbedtls_Stream&`) underlying `market.ws.stream` and
    // `user.ws.stream`. The caller provides them explicitly because
    // Ws_Client doesn't keep a typed pointer to its socket layer.
    //
    // Returns the bitmask of which sources were drained this call.
    template<typename Market_Tls, typename User_Tls>
        requires Has_Pollable_Native<Market_Tls> && Has_Pollable_Native<User_Tls>
    [[nodiscard]] Result<Source_Ready, String_View>
    pump_either(Market_Tls& market_tls, User_Tls& user_tls,
                User_Stream<User_S>& user, u64 timeout_ms,
                i64 now_ms_value) noexcept {
        auto r = poll_either(market_tls, user_tls, timeout_ms);
        if(!r.ok()) return Result<Source_Ready, String_View>::err(spp::move(r.unwrap_err()));
        auto bits = r.unwrap();
        if(ready_market(bits)) {
            auto p = pump_market_once(now_ms_value);
            if(!p.ok()) return Result<Source_Ready, String_View>::err(spp::move(p.unwrap_err()));
        }
        if(ready_user(bits)) {
            auto p = pump_user_once(user);
            if(!p.ok()) return Result<Source_Ready, String_View>::err(spp::move(p.unwrap_err()));
        }
        return Result<Source_Ready, String_View>::ok(spp::move(bits));
    }

    [[nodiscard]] Result<u64, String_View>
    pump_user_once(User_Stream<User_S>& user) noexcept {
        if(reconciler == null) {
            auto msg = user.recv_event();
            if(!msg.ok())
                return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
            return Result<u64, String_View>::ok(0);
        }
        auto msg = user.recv_event();
        if(!msg.ok())
            return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
        auto& bytes = msg.unwrap();
        if(bytes.length() == 0) return Result<u64, String_View>::ok(0);
        String_View body{bytes.data(), bytes.length()};
        return reconciler->apply_event(body);
    }

    // Session-aware user pump: routes through Ws_Session::recv_or_reconnect
    // so reconnects + pong-frame collapse happen for free.
    template<typename Replay, typename Connector>
    [[nodiscard]] Result<u64, String_View>
    pump_user_session_once(
        Ws_Session<User_Stream<User_S>, Replay, Connector>& session,
        i64 now_ms) noexcept {
        if(reconciler == null) {
            auto msg = session.recv_or_reconnect(now_ms);
            if(!msg.ok())
                return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
            return Result<u64, String_View>::ok(0);
        }
        auto msg = session.recv_or_reconnect(now_ms);
        if(!msg.ok())
            return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
        auto& bytes = msg.unwrap();
        if(bytes.length() == 0) return Result<u64, String_View>::ok(0);
        String_View body{bytes.data(), bytes.length()};
        return reconciler->apply_event(body);
    }

    // Multiplexed session-aware pump.  Combines `pump_either`'s fd
    // multiplexer with the session-aware recv path: poll waits for
    // whichever side is ready, calls `maybe_ping` on both before the
    // wait so silent connections stay warm, then dispatches.
    template<typename Market_Tls, typename User_Tls,
             typename Market_Replay, typename User_Replay,
             typename Market_Conn, typename User_Conn>
        requires Has_Pollable_Native<Market_Tls> && Has_Pollable_Native<User_Tls>
    [[nodiscard]] Result<Source_Ready, String_View>
    pump_either_session(
        Market_Tls& market_tls, User_Tls& user_tls,
        Ws_Session<Market_Stream<Market_S>, Market_Replay, Market_Conn>& m_sess,
        Ws_Session<User_Stream<User_S>,   User_Replay,   User_Conn>&   u_sess,
        u64 timeout_ms, i64 now_ms_value) noexcept {
        // Heartbeats first: an idle session must emit a ping inside
        // OKX's 30s window or it'll be dropped before our next poll
        // wakeup.  send_text is fast; do it BEFORE the poll so the
        // ping bytes are in flight while we wait.
        static_cast<void>(m_sess.maybe_ping(now_ms_value));
        static_cast<void>(u_sess.maybe_ping(now_ms_value));

        auto r = poll_either(market_tls, user_tls, timeout_ms);
        if(!r.ok())
            return Result<Source_Ready, String_View>::err(spp::move(r.unwrap_err()));
        auto bits = r.unwrap();
        if(ready_market(bits)) {
            auto p = pump_market_session_once(m_sess, now_ms_value);
            if(!p.ok())
                return Result<Source_Ready, String_View>::err(spp::move(p.unwrap_err()));
        }
        if(ready_user(bits)) {
            auto p = pump_user_session_once(u_sess, now_ms_value);
            if(!p.ok())
                return Result<Source_Ready, String_View>::err(spp::move(p.unwrap_err()));
        }
        return Result<Source_Ready, String_View>::ok(spp::move(bits));
    }

    // (Public so Bar_Trampoline can dispatch back into the driver.)
    void on_bar_(const spp::quant::data::Symbol_Bar& sb) noexcept {
        bars_closed++;
        strategy.running_mode = spp::quant::strategy::Running_Mode::live;
        strategy.x1(sb);
        i64 now = sb.bar.time.unix_ms();
        dispatch_pending_orders_(now);
        dispatch_cancellations_(now);
    }

    // Walk strategy.acc.orders past dispatch_cursor_, ship each pending
    // order to OKX. Successful dispatch advances the cursor whether or
    // not the exchange accepted — the strategy decides re-issue logic.
    void dispatch_pending_orders_(i64 now_ms) noexcept {
        auto& orders = strategy.acc.orders;
        for(u64 i = dispatch_cursor_; i < orders.length(); i++) {
            auto& o = orders[i];
            if(o.status != 0) { dispatch_cursor_ = i + 1; continue; }

            // -- Filter rounding (exchange-rejection prevention) --
            f64 px = spp::quant::data::price_to_f64(o.price);
            f64 qty = o.volume;
            String_View code = o.code.view();
            if(filter_cache != null) {
                px = filter_cache->round_price(code, px);
                qty = filter_cache->round_qty(code, qty);
                if(qty <= 0.0) {
                    dispatch_cursor_ = i + 1;
                    dispatch_failures++;
                    continue;
                }
                if(!filter_cache->notional_ok(code, px, qty)) {
                    dispatch_cursor_ = i + 1;
                    dispatch_failures++;
                    continue;
                }
            }

            using Dir = spp::quant::strategy::Order_Direction;

            // -- Risk gate --
            if(risk_checker != null) {
                quant::risk::Order ro;
                ro.order_id  = o.order_id.clone();
                ro.symbol    = o.code.clone();
                ro.price     = px;
                ro.quantity  = qty;
                ro.side      = (o.direction == Dir::sell ||
                                o.direction == Dir::sell_close ||
                                o.direction == Dir::sell_open) ? 1 : 0;
                ro.timestamp = o.time;
                Vec<quant::risk::Position, A> rps;
                for(u64 pi = 0; pi < strategy.acc.positions.length(); pi++) {
                    auto& sp = strategy.acc.positions[pi];
                    quant::risk::Position rp;
                    rp.symbol = sp.code.clone();
                    rp.quantity = sp.net_volume();
                    rp.market_price = spp::quant::data::price_to_f64(sp.last_price);
                    rps.push(spp::move(rp));
                }
                f64 eq = strategy.acc.total_equity();
                auto rc = risk_checker->check_order(ro, eq, rps);
                if(!rc.ok()) {
                    dispatch_failures++;
                    dispatch_cursor_ = i + 1;
                    continue;
                }
            }

            auto qty_str = detail::decimal_to_string_(
                spp::quant::data::f64_to_price(qty));
            auto price_str = detail::decimal_to_string_(
                spp::quant::data::f64_to_price(px));

            using Dir = spp::quant::strategy::Order_Direction;
            Order_Request req;
            req.instId  = code;
            req.tdMode  = td_mode;
            req.side    = (o.direction == Dir::sell ||
                           o.direction == Dir::sell_close ||
                           o.direction == Dir::sell_open) ? Side::sell : Side::buy;
            req.ordType = Ord_Type::limit;
            req.sz      = qty_str.view();
            req.px      = price_str.view();
            // OKX clOrdId must be alphanumeric (<=32 chars); the account
            // layer's "ORD_<n>" carries an underscore OKX rejects, so map
            // it to an OKX-legal form for the wire only.  Internal maps
            // still key off the original local order_id below.
            auto clord = detail::sanitize_clordid_(o.order_id.view());
            req.clOrdId = clord.view();

            auto resp = App::Okx::place_order(rest, host, signer, limiter, now_ms, req);
            // OKX returns HTTP 200 even for an order it rejected: the
            // top-level `code` is non-"0" and the per-order `sCode` is a
            // non-zero error string with an empty `ordId`.  Treat ONLY a
            // per-order sCode=="0" with a non-empty ordId as a real fill-
            // eligible placement — otherwise the strategy would believe a
            // never-placed order is working.
            bool placed = false;
            if(resp.ok() && resp.unwrap().data.length() > 0) {
                auto& item = resp.unwrap().data[0];
                placed = item.sCode.view() == "0"_v && item.ordId.length() > 0;
            }
            if(placed) {
                orders_dispatched++;
                String<A> ex_id = resp.unwrap().data[0].ordId.clone();
                exchange_ord_id_.insert(o.order_id.clone(), ex_id.clone());
                // Persist the confirmed mapping before advancing.
                if(wal_writer != null) {
                    auto wr = wal_append_confirm(
                        *wal_writer, o.order_id.view(), ex_id.view());
                    if(wr.ok()) wal_last_commit_offset_ = wr.unwrap();
                }
                dispatch_cursor_ = i + 1;
            } else {
                String_View e;
                if(!resp.ok()) {
                    e = resp.unwrap_err();
                } else if(resp.unwrap().data.length() == 0) {
                    e = "no_data_in_response"_v;
                } else {
                    // Per-order rejection — surface OKX's sCode so the
                    // operator can see WHY (insufficient balance, price
                    // band, etc.).  Clone it into an owned field; the
                    // response (and its view) dies at end of iteration.
                    last_dispatch_scode_ = resp.unwrap().data[0].sCode.clone();
                    e = "order_rejected"_v;
                }
                if(e == "backoff_pending"_v) {
                    last_dispatch_err_ = e;
                    break;   // retry on next bar
                }
                dispatch_failures++;
                last_dispatch_err_ = e;
                dispatch_cursor_ = i + 1;
            }
        }
    }

    // Scan all strategy.acc.orders for status==2 (cancelled by strategy)
    // and forward each to the OKX cancel endpoint exactly once.
    void dispatch_cancellations_(i64 now_ms) noexcept {
        auto& orders = strategy.acc.orders;
        for(u64 i = 0; i < orders.length(); i++) {
            auto& o = orders[i];
            if(o.status != 2) continue;
            if(cancel_done_.contains(o.order_id.view())) continue;

            String_View ex = exchange_id_for(o.order_id.view());
            if(ex.length() == 0) {
                // place_order hasn't returned yet. Don't mark done — try
                // again next bar.
                continue;
            }
            auto resp = App::Okx::cancel_order(rest, host, signer, limiter,
                                                now_ms, o.code.view(), ex);
            if(resp.ok()) {
                cancels_dispatched++;
                if(wal_writer != null) {
                    auto wr = wal_append_cancel(*wal_writer, o.order_id.view());
                    if(wr.ok()) wal_last_commit_offset_ = wr.unwrap();
                }
            } else {
                cancel_failures++;
                last_cancel_err_ = resp.unwrap_err();
            }
            cancel_done_.insert(o.order_id.clone(), static_cast<u8>(1));
        }
    }

private:
    // Shared trade-routing logic for both bare-stream and session-aware
    // market pumps.  An empty payload (heartbeat / pong / control)
    // just triggers a `flush_if_due` sweep so quiet markets still emit
    // bar closes on the wall clock.
    [[nodiscard]] Result<u64, String_View>
    route_market_bytes_(Slice<const u8> bytes, i64 now_ms) noexcept {
        if(bytes.length() == 0) {
            for(u64 i = 0; i < aggregators.length(); i++) {
                aggregators[i].flush_if_due(now_ms);
            }
            return Result<u64, String_View>::ok(0);
        }
        String_View body{bytes.data(), bytes.length()};
        u64 cursor = 0;
        Okx_Trade t;
        u64 routed = 0;
        while(next_trade(body, cursor, t)) {
            if(t.instId.length() == 0 || t.px.length() == 0 ||
               t.sz.length() == 0 || t.ts.length() == 0) {
                continue;
            }
            for(u64 i = 0; i < aggregators.length(); i++) {
                if(aggregators[i].symbol.view() != t.instId) continue;
                char buf[256];
                i32 n = Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf),
                    "{\"s\":\"%.*s\",\"p\":\"%.*s\",\"q\":\"%.*s\",\"T\":%.*s}",
                    (i32)t.instId.length(), t.instId.data(),
                    (i32)t.px.length(),     t.px.data(),
                    (i32)t.sz.length(),     t.sz.data(),
                    (i32)t.ts.length(),     t.ts.data());
                // vsnprintf returns the UNtruncated length; a frame whose
                // fields overflow `buf` would make `n >= sizeof(buf)` and
                // an OOB read below.  Drop such a (malformed) trade.
                if(n <= 0 || n >= (i32)sizeof(buf)) break;
                String_View synth{reinterpret_cast<const u8*>(buf),
                                  static_cast<u64>(n)};
                static_cast<void>(aggregators[i].on_message(synth));
                routed++;
                break;
            }
        }
        for(u64 i = 0; i < aggregators.length(); i++) {
            aggregators[i].flush_if_due(now_ms);
        }
        return Result<u64, String_View>::ok(spp::move(routed));
    }
};

} // namespace spp::App::Okx
