#pragma once

// Position reconciler: replays Binance Spot user-data-stream events into a
// local `quant::strategy::Account` so the strategy's view stays in sync
// with what the exchange actually filled.
//
// Two event types matter:
//
// 1. executionReport — fires on every state transition of one of OUR
//    orders. We care about partial / final fills:
//
//      {"e":"executionReport","s":"BTCUSDT","S":"BUY","x":"TRADE",
//       "X":"PARTIALLY_FILLED","l":"0.0005","L":"42000.50","n":"0.0063",
//       "T":1700000000000,"i":12345,"c":"client_id_abc", ...}
//
//    - X = current order status (NEW / PARTIALLY_FILLED / FILLED / ...)
//    - l = lastFilledQty for THIS event
//    - L = lastFilledPrice for THIS event
//    - n = lastFilledCommission (we apply this on top of the synthetic
//          fee Account::make_deal computes; see fee handling below)
//
// 2. outboundAccountPosition — snapshots of free/locked for each asset.
//    We use it as a periodic resync to correct any drift accumulated by
//    skipped/lost executionReports.
//
//      {"e":"outboundAccountPosition","u":1700000000000,
//       "B":[{"a":"USDT","f":"9500.00","l":"500.00"}, ...]}
//
// Both event types are emitted before the matching REST POST /api/v3/order
// returns, so a strategy that submits an order and immediately checks
// position would normally race. The reconciler closes that race by being
// the single source of truth: the strategy reads `account.available` and
// `account.positions[i]` only AFTER `apply_event()` has been called.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/quant/strategy/account.h>
#include <spp/quant/strategy/types.h>

#include <binance/bar_aggregator.h>   // detail::json_field_ / parse_decimal_ / parse_i64_
#include <binance/models.h>

namespace spp::App::Binance {

template<typename A = Mdefault>
struct Position_Reconciler {
    spp::quant::strategy::Account<A>* account = null;
    String_View quote_asset = "USDT"_v;

    // Counters so the live driver can log throughput / detect stalls.
    u64 fills_applied = 0;
    u64 position_snaps_applied = 0;
    u64 events_unknown = 0;

    Position_Reconciler() noexcept = default;
    explicit Position_Reconciler(spp::quant::strategy::Account<A>& acc,
                                  String_View quote = "USDT"_v) noexcept
        : account(&acc), quote_asset(quote) {
    }

    // Dispatch on `e:` and route to the matching handler. Unknown event
    // types are counted but not treated as errors — Binance ships extra
    // payloads (`listStatus`, `balanceUpdate`) that we don't need.
    [[nodiscard]] Result<u64, String_View> apply_event(String_View body) noexcept {
        if(account == null) {
            return Result<u64, String_View>::err("recon_no_account"_v);
        }
        auto ev = detail::json_field_(body, "e"_v);
        if(ev == "executionReport"_v) {
            return apply_execution_(body);
        }
        if(ev == "outboundAccountPosition"_v) {
            return apply_account_position_(body);
        }
        events_unknown++;
        return Result<u64, String_View>::ok(0);
    }

private:
    // Translate a fill into the strategy::Account's bookkeeping. Account
    // doesn't expose a "set balance from external truth" API — its trade
    // log is the source of truth — so we feed each fill through
    // `receive_simpledeal`, which already updates positions, cash, and
    // trade history correctly.
    //
    // Fee handling: Account::make_deal applies a synthetic 0.03% fee on
    // top of every fill. Binance reports the ACTUAL fee in `n`. We adjust
    // `available`/`balance` by the delta so the local cash converges to
    // the exchange's number across many fills, rather than drifting by
    // ~0.03% per trade.
    [[nodiscard]] Result<u64, String_View> apply_execution_(String_View body) noexcept {
        auto status      = detail::json_field_(body, "X"_v);
        auto exec_type   = detail::json_field_(body, "x"_v);
        if(exec_type != "TRADE"_v) {
            // NEW / CANCELED / REJECTED etc. — no fill to apply.
            return Result<u64, String_View>::ok(0);
        }
        if(status != "PARTIALLY_FILLED"_v && status != "FILLED"_v) {
            return Result<u64, String_View>::ok(0);
        }
        auto symbol      = detail::json_field_(body, "s"_v);
        auto side        = detail::json_field_(body, "S"_v);
        auto last_qty_sv = detail::json_field_(body, "l"_v);
        auto last_pr_sv  = detail::json_field_(body, "L"_v);
        auto fee_sv      = detail::json_field_(body, "n"_v);
        if(symbol.length() == 0 || last_qty_sv.length() == 0 ||
           last_pr_sv.length() == 0) {
            return Result<u64, String_View>::err("execReport_missing_field"_v);
        }
        f64 qty   = detail::parse_decimal_(last_qty_sv);
        f64 price = detail::parse_decimal_(last_pr_sv);
        f64 fee_real = fee_sv.length() > 0 ? detail::parse_decimal_(fee_sv) : 0.0;
        if(qty <= 0.0 || price <= 0.0) {
            return Result<u64, String_View>::ok(0);
        }

        // Map BUY/SELL to the strategy enum. Spot accounts only do
        // open-buy / close-sell; futures will need adjusting.
        using Dir    = spp::quant::strategy::Order_Direction;
        using Offset = spp::quant::strategy::Order_Offset;
        Dir dir = side == "SELL"_v ? Dir::sell : Dir::buy;
        Offset off = side == "SELL"_v ? Offset::close_ : Offset::open_;

        Decimal<8> dec_px = spp::quant::data::f64_to_price(price);
        auto trade = account->receive_simpledeal(symbol, dec_px, qty, dir, off);
        (void)trade;

        // Reconcile fee: Account charged 0.03% × notional; replace with
        // the exchange's actual `n`. The sign depends on side: buys debit
        // cash by the fee; sells likewise debit cash by the fee.
        f64 notional = qty * price;
        f64 fee_synth = notional * 0.0003;
        f64 delta = fee_real - fee_synth;
        if(delta != 0.0) {
            account->available -= delta;
            account->balance   -= delta;
        }
        fills_applied++;
        return Result<u64, String_View>::ok(1);
    }

    // Use the snapshot's quote-asset balance to overwrite local cash. The
    // exchange is authoritative here — if we've drifted (lost fill event,
    // restart mid-session), this is what resyncs us.
    //
    // Position quantities for non-quote assets are intentionally NOT
    // overwritten here. The strategy's `Position::volume_long` is in
    // base-asset units and Binance's `f`/`l` doesn't say whether the
    // free balance came from a strategy fill or an external deposit —
    // mapping that needs an asset → symbol lookup we don't yet have.
    // For now we accept that long volumes may drift across external
    // transfers; users should re-init the account if that happens.
    [[nodiscard]] Result<u64, String_View>
    apply_account_position_(String_View body) noexcept {
        // Find the balances array, then scan for our quote asset.
        char pat_b[8];
        pat_b[0] = '"'; pat_b[1] = 'B'; pat_b[2] = '"'; pat_b[3] = ':'; pat_b[4] = 0;
        u64 b_pos = body.length();
        for(u64 i = 0; i + 4 <= body.length(); i++) {
            if(body[i] == '"' && body[i + 1] == 'B' && body[i + 2] == '"' &&
               body[i + 3] == ':') { b_pos = i + 4; break; }
        }
        if(b_pos >= body.length()) return Result<u64, String_View>::ok(0);

        // Step through the array, treating each `{...}` as one balance.
        for(u64 i = b_pos; i < body.length(); i++) {
            if(body[i] != '{') {
                if(body[i] == ']') break;
                continue;
            }
            u64 end = i + 1;
            i32 depth = 1;
            while(end < body.length() && depth > 0) {
                if(body[end] == '{') depth++;
                else if(body[end] == '}') depth--;
                end++;
            }
            String_View entry{body.data() + i, end - i};
            auto asset = detail::json_field_(entry, "a"_v);
            if(asset == quote_asset) {
                auto free_sv   = detail::json_field_(entry, "f"_v);
                auto locked_sv = detail::json_field_(entry, "l"_v);
                f64 free_v   = detail::parse_decimal_(free_sv);
                f64 locked_v = detail::parse_decimal_(locked_sv);
                account->available = free_v;
                account->frozen    = locked_v;
                account->balance   = free_v + locked_v;
                position_snaps_applied++;
                return Result<u64, String_View>::ok(1);
            }
            i = end - 1;
        }
        return Result<u64, String_View>::ok(0);
    }
};

} // namespace spp::App::Binance
