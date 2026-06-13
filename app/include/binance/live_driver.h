#pragma once

// Live trading driver — the glue layer between
//
//   Binance Spot WS market stream   ─►  Bar_Aggregator
//                                       │ (on bar close)
//                                       ▼
//                              Strategy_Base::x1(Symbol_Bar)
//                                       │ (strategy queues orders in acc.orders)
//                                       ▼
//                              translate to Order_Request
//                                       │
//                                       ▼
//                          place_order REST  ──►  Binance
//
// Two streams flow in parallel:
//   * MarketStream — public aggTrade frames feed Bar_Aggregator instances
//   * UserStream   — executionReport / outboundAccountPosition reconcile
//                    into the strategy's local Account via Position_Reconciler
//
// The driver does not own a thread / event loop. Callers decide concurrency:
//   - simplest: foreground loop alternates `pump_market_once` / `pump_user_once`
//   - production: each pump runs in its own Async::Pool task
//
// Order accounting note:
//   Strategy_Base in sim/live mode pushes orders into `acc.orders` with
//   status==0 and does NOT mark them as filled. The driver tracks how many
//   of those it has already forwarded to the exchange via `dispatch_cursor_`
//   so it doesn't double-send when x1() is called repeatedly.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/quant/data/types.h>
#include <spp/quant/strategy/types.h>

#include <binance/api.h>
#include <binance/bar_aggregator.h>
#include <binance/filter_round.h>
#include <spp/quant/risk/risk_checker.h>
#include <binance/market_stream.h>
#include <binance/models.h>
#include <binance/position_recon.h>
#include <binance/rate_limiter.h>
#include <binance/time_sync.h>
#include <binance/user_stream.h>

namespace spp::App::Binance {

namespace detail {

// Decimal<8> → "12345.67890000" for Binance's price/qty wire format. We
// emit up to 8 fractional digits (Binance Spot's max precision); the
// caller's filter validation (LOT_SIZE / PRICE_FILTER) is expected to
// have rounded the value upstream.
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
    // 8 fractional digits, zero-padded.
    char frac_buf[16];
    i32 fn = Libc::snprintf(reinterpret_cast<u8*>(frac_buf), sizeof(frac_buf),
                            "%08llu", static_cast<unsigned long long>(frac));
    for(i32 i = 0; i < fn; i++) buf[n++] = frac_buf[i];
    // Trim trailing zeros (but keep at least one digit after the dot).
    while(n > 0 && buf[n - 1] == '0') n--;
    if(n > 0 && buf[n - 1] == '.') n++; // keep one trailing zero
    String<Mdefault> out(static_cast<u64>(n));
    out.set_length(static_cast<u64>(n));
    Libc::memcpy(out.data(), buf, static_cast<u64>(n));
    return out;
}

} // namespace detail

// Configuration for one symbol bar bucket.
struct Symbol_Spec {
    String_View symbol;       // upper-case, e.g. "BTCUSDT"
    String_View stream_name;  // lower-case stream, e.g. "btcusdt@aggTrade"
    i64 bucket_ms = 60000;    // 1-minute default
};

// `Market_S`, `User_S`, `Rest_S` are independent Byte_Stream types — TLS
// for production, Memory_Stream for tests. `Strategy` is the CRTP derived
// strategy class.
template<typename Strategy, typename Market_S, typename Rest_S,
         typename User_S = Market_S, typename A = Mdefault>
    requires Net::Byte_Stream<Market_S> && Net::Byte_Stream<Rest_S> &&
             Net::Byte_Stream<User_S>
struct Live_Driver {
    Market_Stream<Market_S>& market;
    Rest_S& rest;
    Rate_Limiter& limiter;
    Time_Sync& time;
    Strategy& strategy;
    Position_Reconciler<A>* reconciler = null;

    String_View host        = "api.binance.com"_v;
    String_View api_key     = ""_v;
    String_View api_secret  = ""_v;
    i64 recv_window_ms      = 5000;

    // One aggregator per subscribed symbol. The aggregator's callback
    // is a tiny functor that trampolines back into the driver — keeping
    // the captured state in the trampoline lets every aggregator share
    // a single concrete Bar_Aggregator instantiation.
    struct Bar_Trampoline {
        Live_Driver* self = null;
        void operator()(const spp::quant::data::Symbol_Bar& sb) const noexcept {
            self->on_bar_(sb);
        }
    };
    Vec<Bar_Aggregator<Bar_Trampoline>, A> aggregators;

    // Diagnostic counters / cursors.
    u64 bars_closed = 0;
    u64 orders_dispatched = 0;
    u64 dispatch_failures = 0;
    u64 cancels_dispatched = 0;
    u64 cancel_failures = 0;
    u64 dispatch_cursor_ = 0;  // index into strategy.acc.orders (place path)

    // Local order_id (strategy's bookkeeping) → exchange orderId. Cancel
    // and query paths need the exchange-side ID, while everything inside
    // the strategy speaks the local one. Populated from each successful
    // place_order response; the executionReport path can also read it
    // back when the user-stream payload echoes our clientOrderId.
    Map<String<A>, i64, A> exchange_order_id_;

    // Order ids we've already submitted a cancel REST for. Idempotency
    // guard so a strategy that holds an order at status==2 doesn't get
    // its cancel re-dispatched every bar.
    Map<String<A>, u8, A> cancel_done_;

    // Diagnostics: error string from the most recent failed cancel,
    // surface so tests / operators can see why a REST call rejected.
    String_View last_cancel_err_;
    String_View last_dispatch_err_;

    // Optional symbol-filter cache.  When set (non-null), every order's
    // price and quantity are rounded to the exchange's tickSize / stepSize
    // BEFORE dispatch; orders whose quantity rounds to zero or whose
    // notional falls below the symbol's MIN_NOTIONAL are counted as
    // filtered and never submitted.  Leave null to bypass (tests).
    Filter_Cache<A>* filter_cache = null;
    quant::risk::Risk_Checker<A>* risk_checker = null;

    Live_Driver(Market_Stream<Market_S>& m, Rest_S& r, Rate_Limiter& lim,
                Time_Sync& t, Strategy& s) noexcept
        : market(m), rest(r), limiter(lim), time(t), strategy(s) {
    }

    void attach_reconciler(Position_Reconciler<A>& rec) noexcept {
        reconciler = &rec;
    }

    // Allocate an aggregator for `symbol` so on_message can route trades
    // to the right bucket. Subscriptions to the WS stream remain the
    // caller's responsibility (call `market.subscribe(...)`).
    void register_symbol(String_View symbol, i64 bucket_ms = 60000) noexcept {
        aggregators.push(Bar_Aggregator<Bar_Trampoline>{
            symbol, bucket_ms, Bar_Trampoline{this}});
    }

    // Pull one message from the market stream, route through the matching
    // aggregator. Returns the number of bars closed by this message (0 or
    // 1 for aggTrade frames).
    [[nodiscard]] Result<u64, String_View> pump_market_once(i64 now_ms) noexcept {
        auto msg = market.recv();
        if(!msg.ok()) {
            return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
        }
        auto& bytes = msg.unwrap();
        if(bytes.length() == 0) {
            // Heartbeat/empty — just check timers.
            for(u64 i = 0; i < aggregators.length(); i++) {
                aggregators[i].flush_if_due(now_ms);
            }
            return Result<u64, String_View>::ok(0);
        }
        String_View body{bytes.data(), bytes.length()};

        // Find the aggregator for this trade's symbol and feed it. If
        // none matches, silently drop — the strategy didn't ask for it.
        auto sym = detail::json_field_(body, "s"_v);
        for(u64 i = 0; i < aggregators.length(); i++) {
            if(aggregators[i].symbol.view() == sym) {
                auto r = aggregators[i].on_message(body);
                if(!r.ok()) return Result<u64, String_View>::err(
                    spp::move(r.unwrap_err()));
                break;
            }
        }
        // Opportunistic timer flush so a quiet symbol still emits closes.
        for(u64 i = 0; i < aggregators.length(); i++) {
            aggregators[i].flush_if_due(now_ms);
        }
        return Result<u64, String_View>::ok(0);
    }

    // Pull one user-stream event and apply it through the reconciler. No-
    // ops if no reconciler is attached (e.g. read-only market sessions).
    [[nodiscard]] Result<u64, String_View>
    pump_user_once(User_Stream<User_S>& user) noexcept {
        if(reconciler == null) {
            // Drain and discard.
            auto msg = user.recv_event();
            if(!msg.ok()) return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
            return Result<u64, String_View>::ok(0);
        }
        auto msg = user.recv_event();
        if(!msg.ok()) return Result<u64, String_View>::err(spp::move(msg.unwrap_err()));
        auto& bytes = msg.unwrap();
        if(bytes.length() == 0) return Result<u64, String_View>::ok(0);
        String_View body{bytes.data(), bytes.length()};
        return reconciler->apply_event(body);
    }

    // (Public so Bar_Trampoline can dispatch back into the driver.)
    void on_bar_(const spp::quant::data::Symbol_Bar& sb) noexcept {
        bars_closed++;
        strategy.running_mode = spp::quant::strategy::Running_Mode::live;
        strategy.x1(sb);
        i64 now_ms = sb.bar.time.unix_ms();
        dispatch_pending_orders_(now_ms);
        dispatch_cancellations_(now_ms);
    }

    // Look up the exchange-side orderId for a given local order_id. Returns
    // 0 if the order hasn't been confirmed by the exchange yet (no fill
    // event, no place_order success).
    [[nodiscard]] i64 exchange_id_for(String_View local_id) const noexcept {
        auto e = exchange_order_id_.try_get(local_id);
        return e.ok() ? **e : 0;
    }

    // Walk the strategy's pending orders past the dispatch cursor and
    // forward each to the exchange. Successful dispatches advance the
    // cursor regardless of REST status — we don't want to retry the same
    // order on the next bar if it was rejected mid-flight; the strategy
    // will issue a fresh one if needed.
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
                    // Rounded-to-zero means the size is below LOT_SIZE
                    // minQty — skip this order as unplaceable.  Advance
                    // cursor so we don't retry it forever.
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
                    rp.market_price = quant::data::price_to_f64(sp.last_price);
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

            Order_Request req;
            req.symbol = code;
            req.side = (o.direction == Dir::sell ||
                        o.direction == Dir::sell_close ||
                        o.direction == Dir::sell_open) ? Side::SELL : Side::BUY;
            req.type = Order_Type::LIMIT;
            req.time_in_force = Opt<Time_In_Force>{Time_In_Force::GTC};
            req.quantity = qty_str.view();
            req.price = price_str.view();
            req.client_order_id = o.order_id.view();

            auto resp = place_order(rest, host, api_key, api_secret,
                                    limiter, time, now_ms, req, recv_window_ms);
            if(resp.ok()) {
                orders_dispatched++;
                exchange_order_id_.insert(o.order_id.clone(),
                                          resp.unwrap().orderId);
                dispatch_cursor_ = i + 1;
            } else {
                String_View e = resp.unwrap_err();
                if(e == "backoff_pending"_v) {
                    // Tls_Session reconnection is in its waiting window —
                    // leave the order at cursor so it retries on the
                    // next bar rather than being forgotten.
                    last_dispatch_err_ = e;
                    break;  // don't process further orders until backoff clears
                }
                dispatch_failures++;
                last_dispatch_err_ = e;
                dispatch_cursor_ = i + 1;
            }
        }
    }

    // Walk every cancelled-by-strategy order (status==2) and forward
    // each to the exchange once. Idempotency via `cancel_done_`: orders
    // we've already cancelled stay marked so a strategy that leaves a
    // cancelled order in its log doesn't trigger re-cancels every bar.
    void dispatch_cancellations_(i64 now_ms) noexcept {
        auto& orders = strategy.acc.orders;
        for(u64 i = 0; i < orders.length(); i++) {
            auto& o = orders[i];
            if(o.status != 2) continue;
            if(cancel_done_.contains(o.order_id.view())) continue;

            i64 ex_id = exchange_id_for(o.order_id.view());
            if(ex_id <= 0) {
                // No exchange-side handle yet — either place_order never
                // succeeded or it hasn't returned. Leave un-marked so we
                // retry once the exchange ID lands.
                continue;
            }

            auto resp = cancel_order(rest, host, api_key, api_secret,
                                     limiter, time, now_ms,
                                     o.code.view(), ex_id, recv_window_ms);
            if(resp.ok()) {
                cancels_dispatched++;
            } else {
                cancel_failures++;
                last_cancel_err_ = spp::move(resp.unwrap_err());
            }
            // Mark either way: a failed cancel on a non-existent order
            // (already filled / already gone) shouldn't be retried.
            cancel_done_.insert(o.order_id.clone(), static_cast<u8>(1));
        }
    }
};

} // namespace spp::App::Binance
