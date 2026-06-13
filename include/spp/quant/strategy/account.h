#pragma once

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/containers/vec.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/numeric/math.h>
#include <spp/quant/strategy/types.h>

namespace spp::quant::strategy {

// ============================================================
// Account — portfolio and order management engine
// Tracks cash, positions, orders, and trade history.
// ============================================================

template <typename A = Mdefault>
struct Account {
    String<A> account_id;
    f64 initial_cash = 0.0;
    f64 available = 0.0;
    f64 frozen = 0.0;
    f64 balance = 0.0;
    Vec<Position, A> positions;
    Vec<Order, A> orders;
    Vec<Trade, A> trades;
    Vec<Account_Snapshot, A> snapshots;
    Market_Type market_type = Market_Type::stock_cn;

    u64 order_counter_ = 0;
    u64 trade_counter_ = 0;

    Account() noexcept = default;

    Account(String<A> id, f64 cash) noexcept
        : account_id(spp::move(id)), initial_cash(cash), available(cash), balance(cash) {
    }

    // ---- Order Management ----

    /// Send an order. Freezes funds/positions and records the order.
    /// Returns the created Order on success, or an error string.
    auto send_order(Order& order) -> Result<Order, String_View> {
        // Generate order ID if empty
        if (order.order_id.empty()) {
            order.order_id = _make_order_id();
        }

        // Validate price
        if (order.price.raw() <= 0) {
            return Result<Order, String_View>::err("price_must_be_positive"_v);
        }

        // Validate volume
        if (order.volume <= 0.0) {
            return Result<Order, String_View>::err("volume_must_be_positive"_v);
        }

        // Calculate required funds
        f64 required = order.volume * _decimal_to_f64(order.price);

        // For buy orders, check and freeze cash
        if (order.direction == Order_Direction::buy ||
            order.direction == Order_Direction::buy_open) {

            if (required > available) {
                return Result<Order, String_View>::err("insufficient_funds"_v);
            }
            available -= required;
            frozen += required;
        }
        // For sell orders, check position availability
        else if (order.direction == Order_Direction::sell ||
                 order.direction == Order_Direction::sell_close) {

            auto pos_opt = get_position(order.code.view());
            f64 available_vol = 0.0;
            if (pos_opt.ok()) {
                if (order.offset == Order_Offset::close_ ||
                    order.direction == Order_Direction::sell_close) {
                    available_vol = pos_opt->volume_long;
                } else {
                    available_vol = pos_opt->volume_long; // selling from long
                }
            }
            if (order.volume > available_vol) {
                return Result<Order, String_View>::err("insufficient_position"_v);
            }
        }
        // For sell_open / buy_close in futures mode
        else if (order.direction == Order_Direction::sell_open) {
            if (required > available) {
                return Result<Order, String_View>::err("insufficient_margin"_v);
            }
            available -= required;
            frozen += required;
        }

        order.status = 0; // pending
        orders.push(Order{order.order_id.clone(),
                           order.code.clone(),
                           order.direction,
                           order.offset,
                           order.price,
                           order.volume,
                           order.time,
                           order.status});

        return Result<Order, String_View>::ok(Order{
            orders[orders.length() - 1].order_id.clone(),
            order.code.clone(),
            order.direction,
            order.offset,
            order.price,
            order.volume,
            order.time,
            order.status});
    }

    /// Cancel a pending order. Unfreezes funds and marks as cancelled.
    auto cancel_order(String_View order_id) -> Result<Order, String_View> {
        for (u64 i = 0; i < orders.length(); i++) {
            if (_view_eq(orders[i].order_id.view(), order_id) && orders[i].status == 0) {
                // Unfreeze
                f64 required = orders[i].volume * _decimal_to_f64(orders[i].price);
                frozen -= required;
                available += required;

                orders[i].status = 2; // cancelled
                return Result<Order, String_View>::ok(Order{
                    orders[i].order_id.clone(),
                    orders[i].code.clone(),
                    orders[i].direction,
                    orders[i].offset,
                    orders[i].price,
                    orders[i].volume,
                    orders[i].time,
                    orders[i].status});
            }
        }
        return Result<Order, String_View>::err("order_not_found"_v);
    }

    /// Fill an order at its current price. Creates a trade and updates positions.
    auto make_deal(Order& order) -> Trade {
        f64 required = order.volume * _decimal_to_f64(order.price);
        f64 fee = required * 0.0003; // default 0.03% commission

        String<A> trade_id = _make_trade_id();

        // Free previously frozen funds
        frozen -= required;

        Trade trade{trade_id.clone(),
                     order.order_id.clone(),
                     order.code.clone(),
                     order.direction,
                     order.offset,
                     order.price,
                     order.volume,
                     fee,
                     order.time};

        order.status = 1; // filled

        // Update balance and available
        if (order.direction == Order_Direction::buy ||
            order.direction == Order_Direction::buy_open) {
            balance -= fee;
            available -= fee;
        } else if (order.direction == Order_Direction::sell ||
                   order.direction == Order_Direction::sell_close) {
            available += required - fee;
            balance = available + frozen;
        } else if (order.direction == Order_Direction::sell_open) {
            balance -= fee;
            available -= fee;
        } else if (order.direction == Order_Direction::buy_close) {
            available -= required + fee;
            balance = available + frozen;
        }

        // Update positions
        _update_position(order);

        trades.push(spp::move(trade));
        return Trade{trades[trades.length() - 1].trade_id.clone(),
                      trades[trades.length() - 1].order_id.clone(),
                      trades[trades.length() - 1].code.clone(),
                      trades[trades.length() - 1].direction,
                      trades[trades.length() - 1].offset,
                      trades[trades.length() - 1].price,
                      trades[trades.length() - 1].volume,
                      trades[trades.length() - 1].fee,
                      trades[trades.length() - 1].time};
    }

    /// Receive a simple deal (externally confirmed trade).
    auto receive_simpledeal(String_View code, Decimal<8> price, f64 volume,
                             Order_Direction dir, Order_Offset offset) -> Trade {
        Order temp_order;
        temp_order.code = code.string<A>();
        temp_order.price = price;
        temp_order.volume = volume;
        temp_order.direction = dir;
        temp_order.offset = offset;
        temp_order.time = Deterministic_Time{};
        return make_deal(temp_order);
    }

    // ---- Position Queries ----

    /// Get the position for a given instrument code.
    auto get_position(String_View code) const -> Opt<Position> {
        for (u64 i = 0; i < positions.length(); i++) {
            if (_view_eq(positions[i].code.view(), code)) {
                return Opt<Position>{Position{positions[i].code.clone(),
                                               positions[i].volume_long,
                                               positions[i].volume_short,
                                               positions[i].open_price_long,
                                               positions[i].open_price_short,
                                               positions[i].last_price,
                                               positions[i].float_profit,
                                               positions[i].margin_used}};
            }
        }
        return Opt<Position>{};
    }

    // ---- Mark-to-Market ----

    /// Update unrealized PnL when prices change.
    auto on_price_change(String_View code, Decimal<8> new_price) -> void {
        for (u64 i = 0; i < positions.length(); i++) {
            if (!_view_eq(positions[i].code.view(), code)) continue;

            positions[i].last_price = new_price;

            // Long PnL
            if (positions[i].volume_long > 0.0) {
                f64 open_val = positions[i].volume_long * _decimal_to_f64(positions[i].open_price_long);
                f64 curr_val = positions[i].volume_long * _decimal_to_f64(new_price);
                positions[i].float_profit = curr_val - open_val;
            }

            // Short PnL
            if (positions[i].volume_short > 0.0) {
                f64 open_val = positions[i].volume_short * _decimal_to_f64(positions[i].open_price_short);
                f64 curr_val = positions[i].volume_short * _decimal_to_f64(new_price);
                positions[i].float_profit += open_val - curr_val;
            }
            return;
        }
    }

    // ---- Daily Settlement ----
    /// Mark-to-market all positions and reset daily PnL tracking.
    auto settle() -> void {
        // No daily reset in simplified model — in full implementation:
        // - Close-and-reopen futures positions
        // - Reset daily PnL
        // - Add margin calls
        Account_Snapshot snap;
        snap.time = Deterministic_Time{};
        snap.balance = balance;
        snap.available = available;
        snap.frozen = frozen;
        snap.equity = total_equity();
        snap.total_pnl = total_equity() - initial_cash;
        snapshots.push(spp::move(snap));
    }

    // ---- Aggregate Metrics ----

    /// Total equity = balance + unrealized PnL across all positions.
    auto total_equity() const -> f64 {
        f64 eq = balance;
        for (u64 i = 0; i < positions.length(); i++) {
            eq += positions[i].float_profit;
        }
        return eq;
    }

    /// Margin ratio = total_equity / total_margin_used.
    /// Values below 1.0 indicate risk of margin call.
    auto margin_ratio() const -> f64 {
        f64 total_margin = 0.0;
        for (u64 i = 0; i < positions.length(); i++) {
            total_margin += positions[i].margin_used;
        }
        if (total_margin <= 0.0) return 1.0e9; // effectively infinite
        return total_equity() / total_margin;
    }

    // ---- Float Profit Sum ----
    auto total_float_profit() const -> f64 {
        f64 fp = 0.0;
        for (u64 i = 0; i < positions.length(); i++) {
            fp += positions[i].float_profit;
        }
        return fp;
    }

private:
    // ---- Internal Helpers ----

    auto _make_order_id() -> String<A> {
        char buf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf), "ORD_%llu",
                       static_cast<unsigned long long>(++order_counter_));
        return _cstr_to_string(buf);
    }

    auto _make_trade_id() -> String<A> {
        char buf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf), "TRD_%llu",
                       static_cast<unsigned long long>(++trade_counter_));
        return _cstr_to_string(buf);
    }

    static auto _cstr_to_string(const char* s) -> String<A> {
        u64 len = Libc::strlen(s);
        String<A> result(len);
        result.set_length(len);
        Libc::memcpy(result.data(), s, len);
        return result;
    }

    static auto _view_eq(String_View a, String_View b) noexcept -> bool {
        if (a.length() != b.length()) return false;
        return Libc::strncmp(reinterpret_cast<const char*>(a.data()),
                              reinterpret_cast<const char*>(b.data()),
                              a.length()) == 0;
    }

    static auto _decimal_to_f64(Decimal<8> d) noexcept -> f64 {
        return static_cast<f64>(d.raw()) / static_cast<f64>(Decimal<8>::factor());
    }

    /// Update (or create) position after a trade.
    auto _update_position(const Order& order) -> void {
        bool is_open = (order.offset == Order_Offset::open_);
        bool is_long = (order.direction == Order_Direction::buy ||
                        order.direction == Order_Direction::buy_open ||
                        order.direction == Order_Direction::sell_close);

        // Find existing position
        u64 pos_idx = positions.length(); // sentinel
        for (u64 i = 0; i < positions.length(); i++) {
            if (_view_eq(positions[i].code.view(), order.code.view())) {
                pos_idx = i;
                break;
            }
        }

        if (pos_idx == positions.length()) {
            // Create new position
            Position new_pos;
            new_pos.code = order.code.clone();
            if (is_open && is_long) {
                new_pos.volume_long = order.volume;
                new_pos.open_price_long = order.price;
            } else if (is_open && !is_long) {
                new_pos.volume_short = order.volume;
                new_pos.open_price_short = order.price;
            } else if (!is_open && is_long) {
                // buy_close with no position is an error case, ignore
                return;
            }
            new_pos.last_price = order.price;
            positions.push(spp::move(new_pos));
            return;
        }

        // Update existing position
        Position& pos = positions[pos_idx];
        if (is_open) {
            if (is_long) {
                // Weighted average open price for adds to long
                f64 total_vol = pos.volume_long + order.volume;
                f64 new_open = (pos.volume_long * _decimal_to_f64(pos.open_price_long) +
                                order.volume * _decimal_to_f64(order.price)) / total_vol;
                pos.open_price_long = _f64_to_decimal(new_open);
                pos.volume_long = total_vol;
            } else {
                f64 total_vol = pos.volume_short + order.volume;
                f64 new_open = (pos.volume_short * _decimal_to_f64(pos.open_price_short) +
                                order.volume * _decimal_to_f64(order.price)) / total_vol;
                pos.open_price_short = _f64_to_decimal(new_open);
                pos.volume_short = total_vol;
            }
        } else {
            // Closing
            if (is_long) {
                // buy_close = cover short
                pos.volume_short = Math::max(0.0, pos.volume_short - order.volume);
            } else {
                // sell_close = reduce long
                f64 close_vol = Math::min(pos.volume_long, order.volume);
                pos.volume_long -= close_vol;
                // If fully closed, reset open price
                if (pos.volume_long <= 0.0) {
                    pos.volume_long = 0.0;
                    pos.open_price_long = Decimal<8>{};
                }
            }
        }
        pos.last_price = order.price;
        pos.margin_used = pos.gross_volume() * _decimal_to_f64(order.price) * 0.1;
    }

    static auto _f64_to_decimal(f64 v) noexcept -> Decimal<8> {
        i64 raw = static_cast<i64>(v * static_cast<f64>(Decimal<8>::factor()));
        return Decimal<8>::from_raw(raw);
    }
};

} // namespace spp::quant::strategy
