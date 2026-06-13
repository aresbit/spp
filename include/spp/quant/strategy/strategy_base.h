#pragma once

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/containers/vec.h>
#include <spp/containers/map.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/numeric/math.h>
#include <spp/core/deterministic.h>
#include <spp/quant/data/types.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/data/tick_data.h>
#include <spp/quant/strategy/types.h>
#include <spp/quant/strategy/account.h>

namespace spp::quant::strategy {

// ============================================================
// Strategy_Base — CRTP strategy pattern base class
//
// Usage:
//   struct MyStrategy : Strategy_Base<MyStrategy, Mdefault> {
//       void on_bar(const Bar& bar) override { ... }
//   };
//
// The engine drives the strategy by calling x1() for each bar.
// Derived classes override the on_* hooks to implement logic.
// ============================================================

template <typename Derived, typename A = Mdefault>
struct Strategy_Base {
    // === Configuration ===
    String<A> strategy_id;
    String<A> frequency;          // "tick", "1min", "5min", "day"
    Vec<String<A>, A> codes;      // instrument codes traded
    Running_Mode running_mode = Running_Mode::backtest;
    Market_Type market_type = Market_Type::stock_cn;
    f64 init_cash = 100000.0;
    Account<A> acc;

    // === State ===
    Map<String<A>, Decimal<8>, A> latest_price;
    Vec<Strategy_Signal, A> signals;
    Map<String<A>, f64, A> system_vars;   // temporary signal store (cleared after 1min)
    Vec<Account_Snapshot, A> account_snapshots;
    Deterministic_Time running_time;
    u64 bar_id = 0;
    u64 day_counter = 0;

    // Anti-duplicate order tracking: code -> (direction -> offset)
    Map<String<A>, Map<Order_Direction, Order_Offset, A>, A> last_order_towards;
    // Tracks whether an order was placed in the current: direction_offset_order
    Map<String<A>, u64, A> bar_orders;  // direction_offset -> bar_id

    // Day tracking for on_dailyclose / on_dailyopen
    i64 last_trading_day_ = 0;

    // Live event-loop run flag (see run() / stop()).
    bool live_running_ = false;

    // === Constructors ===

    Strategy_Base() noexcept = default;

    // NOTE: this constructor reads `id` after moving from it; preserve
    // a clone before the move-into strategy_id.
    Strategy_Base(String<A> id, f64 cash) noexcept
        : strategy_id(id.clone()), init_cash(cash),
          acc(spp::move(id), cash) {
        // Derived classes are responsible for calling _derived().user_init()
        // from their own constructor body — calling it here would force the
        // Derived class to be complete at this point, which it isn't under
        // CRTP first-instantiation.
    }

    // === Derived accessor ===
    // Templated so the body is only instantiated when actually called, after
    // Derived has been completed. Plain in-class non-template members trigger
    // CRTP order-of-definition errors under explicit template instantiation.

    template<typename Self = Derived>
    auto _derived() -> Self& { return *static_cast<Self*>(this); }
    template<typename Self = Derived>
    auto _derived() const -> const Self& { return *static_cast<const Self*>(this); }

    // === Virtual Hooks (overridable by derived class) ===

    void user_init() {}
    void on_bar(const data::Bar& bar) {}
    void on_tick(const data::Tick& tick) {}
    void on_1min_bar() {}
    void on_5min_bar() {}
    void on_15min_bar() {}
    void on_30min_bar() {}
    void on_dailyopen() {}
    void on_dailyclose() {}
    void on_deal(const Trade& trade) {}
    void risk_check() {}

    void on_ordererror(Order_Direction /*dir*/, Order_Offset /*offset*/,
                       Decimal<8> /*price*/, f64 /*volume*/) {}

    // === Properties ===

    auto get_code() const -> String_View {
        return strategy_id.view();
    }

    /// Number of bars since long entry (0 if no position).
    auto BarsSinceEntryLong() const -> u64 {
        for (u64 i = 0; i < acc.positions.length(); i++) {
            if (acc.positions[i].volume_long > 0.0) {
                // Return bars since last trade for this code
                for (u64 j = acc.trades.length(); j > 0; j--) {
                    u64 tidx = j - 1;
                    if (_sv_eq(acc.trades[tidx].code.view(), acc.positions[i].code.view())) {
                        if (acc.trades[tidx].direction == Order_Direction::buy ||
                            acc.trades[tidx].direction == Order_Direction::buy_open) {
                            return bar_id - _bar_id_at_time(acc.trades[tidx].time);
                        }
                    }
                }
                return bar_id; // position existed before backtest start
            }
        }
        return 0;
    }

    auto BarsSinceEntryShort() const -> u64 {
        for (u64 i = 0; i < acc.positions.length(); i++) {
            if (acc.positions[i].volume_short > 0.0) {
                for (u64 j = acc.trades.length(); j > 0; j--) {
                    u64 tidx = j - 1;
                    if (_sv_eq(acc.trades[tidx].code.view(), acc.positions[i].code.view())) {
                        if (acc.trades[tidx].direction == Order_Direction::sell ||
                            acc.trades[tidx].direction == Order_Direction::sell_open) {
                            return bar_id - _bar_id_at_time(acc.trades[tidx].time);
                        }
                    }
                }
                return bar_id;
            }
        }
        return 0;
    }

    auto EntryPriceLong() const -> Opt<Decimal<8>> {
        for (u64 i = 0; i < acc.positions.length(); i++) {
            if (acc.positions[i].volume_long > 0.0) {
                return Opt<Decimal<8>>{acc.positions[i].open_price_long};
            }
        }
        return {};
    }

    auto EntryPriceShort() const -> Opt<Decimal<8>> {
        for (u64 i = 0; i < acc.positions.length(); i++) {
            if (acc.positions[i].volume_short > 0.0) {
                return Opt<Decimal<8>>{acc.positions[i].open_price_short};
            }
        }
        return {};
    }

    // === Signal Recording ===

    void plot(String_View name, f64 value, String_View format = "line"_v) {
        Strategy_Signal sig;
        sig.time = running_time;
        sig.name = name.string<A>();
        sig.value = value;
        sig.format = format.string<A>();
        signals.push(spp::move(sig));
    }

    // === Order Management ===

    /// Send order with explicit code.
    auto send_order(Order_Direction dir, Order_Offset offset,
                     Decimal<8> price, f64 volume,
                     String_View code) -> Result<Order, String_View> {

        if (running_mode == Running_Mode::backtest) {
            return _send_order_backtest(dir, offset, price, volume, code);
        } else {
            // sim / live mode: validate, send, wait for confirmation
            return _send_order_sim(dir, offset, price, volume, code);
        }
    }

    /// Send order with default code (first in codes list).
    auto send_order(Order_Direction dir, Order_Offset offset,
                     Decimal<8> price, f64 volume) -> Result<Order, String_View> {
        String_View code = codes.length() > 0 ? codes[0].view() : ""_v;
        return send_order(dir, offset, price, volume, code);
    }

    // === Convenience Order Methods ===

    auto buy_open(Decimal<8> price, f64 volume, String_View code) -> Result<Order, String_View> {
        return send_order(Order_Direction::buy_open, Order_Offset::open_, price, volume, code);
    }

    auto sell_open(Decimal<8> price, f64 volume, String_View code) -> Result<Order, String_View> {
        return send_order(Order_Direction::sell_open, Order_Offset::open_, price, volume, code);
    }

    auto buy_close(Decimal<8> price, f64 volume, String_View code) -> Result<Order, String_View> {
        return send_order(Order_Direction::buy_close, Order_Offset::close_, price, volume, code);
    }

    auto sell_close(Decimal<8> price, f64 volume, String_View code) -> Result<Order, String_View> {
        return send_order(Order_Direction::sell_close, Order_Offset::close_, price, volume, code);
    }

    /// Force close all positions for a code.
    auto force_close(String_View code) -> void {
        auto pos_opt = acc.get_position(code);
        if (!pos_opt.ok()) return;

        auto& pos = *pos_opt;
        if (pos.volume_long > 0.0) {
            auto price_opt = _get_latest_price(code);
            if (price_opt.ok()) {
                static_cast<void>(sell_close(*price_opt, pos.volume_long, code));
            }
        }
        if (pos.volume_short > 0.0) {
            auto price_opt = _get_latest_price(code);
            if (price_opt.ok()) {
                static_cast<void>(buy_close(*price_opt, pos.volume_short, code));
            }
        }
    }

    /// Cancel all pending orders.
    auto cancel_all() -> void {
        for (u64 i = 0; i < acc.orders.length(); i++) {
            if (acc.orders[i].status == 0) {
                static_cast<void>(acc.cancel_order(acc.orders[i].order_id.view()));
            }
        }
    }

    // === Order Validation ===

    /// Check if an order direction/offset for a code is valid right now.
    auto check_order(Order_Direction dir, Order_Offset offset, String_View code) -> bool {
        // Prevent duplicate orders in the same bar
        String<A> key = _make_direction_offset_key(dir, offset);
        String<A> full_key = key.clone();
        // Build composite key: code + "|" + dir_offset
        // For simplicity, check if code key appears in bar_orders
        // and if the direction_offset for that code was already sent this bar

        auto code_entry = last_order_towards.try_get(code.template string<A>());
        if (code_entry.ok()) {
            auto dir_entry = (**code_entry).try_get(dir);
            if (dir_entry.ok() && **dir_entry == offset) {
                // Check if this was sent this bar
                auto bo = bar_orders.try_get(full_key.view());
                if (bo.ok() && **bo == bar_id) {
                    return false; // duplicate order this bar
                }
            }
        }
        return true;
    }

    // === Position Management ===

    auto get_positions(String_View code) -> Position {
        auto pos_opt = acc.get_position(code);
        if (pos_opt.ok()) {
            const auto& src = *pos_opt;
            return Position{src.code.clone(),
                            src.volume_long, src.volume_short,
                            src.open_price_long, src.open_price_short,
                            src.last_price,
                            src.float_profit, src.margin_used};
        }
        Position empty;
        empty.code = code.template string<A>();
        return empty;
    }

    auto get_cash() -> f64 {
        return acc.available;
    }

    /// Record current account state as a snapshot.
    auto update_account() -> void {
        Account_Snapshot snap;
        snap.time = running_time;
        snap.balance = acc.balance;
        snap.available = acc.available;
        snap.frozen = acc.frozen;
        snap.equity = acc.total_equity();
        snap.total_pnl = acc.total_equity() - init_cash;
        account_snapshots.push(spp::move(snap));
    }

    // === Entry Points ===

    /// Backtest with ohlcv data (debug mode — returns nothing).
    auto debug(data::Ohlcv_Data<A>& data) -> void {
        run_backtest(data);
    }

    /// Run full backtest and return the account. Drives the strategy bar by
    /// bar — Backtest_Engine offers the richer rebalancing loop; this entry
    /// point is the minimal "feed bars to on_bar()" form.
    auto run_backtest(data::Ohlcv_Data<A>& data) -> Account<A> {
        for (u64 i = 0; i < data.bars.length(); i++) {
            x1(data.bars[i]);
        }
        Account<A> out;
        out.account_id = acc.account_id.clone();
        out.initial_cash = acc.initial_cash;
        out.balance = acc.balance;
        out.available = acc.available;
        out.frozen = acc.frozen;
        return out;
    }

    /// Live/paper trading event loop. `next` is a pull source that returns
    /// the next market bar as `Opt<data::Symbol_Bar>`; an empty Opt means the
    /// feed is drained and the loop exits cleanly. Each bar drives the full
    /// x1() pipeline followed by one risk_check() pass. A hook (on_bar,
    /// risk_check, …) may call stop() to break out after the current bar.
    /// Returns the number of bars processed.
    template <typename Next>
    auto run(Next&& next) -> u64 {
        live_running_ = true;
        u64 processed = 0;
        while (live_running_) {
            auto ev = next();
            if (!ev.ok()) break;          // feed closed → clean shutdown
            x1(*ev);
            _derived().risk_check();
            processed++;
        }
        live_running_ = false;
        return processed;
    }

    /// Signal the live event loop to stop after the current iteration. Safe to
    /// call from any on_* / risk_check hook.
    auto stop() -> void { live_running_ = false; }

    /// Whether the live event loop is currently running.
    auto is_running() const -> bool { return live_running_; }

    // === Internal: Process one bar row (THE ENGINE ENTRY POINT) ===
    //
    // x1() replicates the exact Python QA execution order:
    //   1. Update latest_price[code] = close
    //   2. Day change detection → on_dailyclose() → on_dailyopen() → acc.settle()
    //   3. on_1min_bar() (copies system_vars to signals)
    //   4. Append market data
    //   5. Advance running_time
    //   6. Call self.on_bar(item)  (the derived class logic)
    //   7. Record account snapshot

    auto x1(const data::Symbol_Bar& item) -> void {
        String_View code = item.symbol.view();
        bar_id++;

        // Step 1: Update latest price
        latest_price.insert(code.string<A>(), item.bar.close);

        // Step 2: Day change detection
        i64 current_day = item.bar.time.unix_ns() / (24LL * 3600LL * 1000000000LL);
        if (last_trading_day_ != 0 && current_day != last_trading_day_) {
            _derived().on_dailyclose();
            acc.settle();
            day_counter++;
            _derived().on_dailyopen();
        }
        last_trading_day_ = current_day;

        // Step 3: Minute boundary hooks
        i64 minute_of_day = (item.bar.time.unix_ns() / 60000000000LL) % 1440;
        static i64 last_minute_ = -1;
        if (minute_of_day != last_minute_) {
            _derived().on_1min_bar();
            // Copy system_vars to signals at 1min boundaries (Python QA behavior)
            _flush_system_vars();

            // 5-min boundary
            if (minute_of_day % 5 == 0) _derived().on_5min_bar();
            // 15-min boundary
            if (minute_of_day % 15 == 0) _derived().on_15min_bar();
            // 30-min boundary
            if (minute_of_day % 30 == 0) _derived().on_30min_bar();

            last_minute_ = minute_of_day;
        }

        // Step 4: Update running time
        running_time = item.bar.time;

        // Step 5: Mark-to-market for this code
        acc.on_price_change(code, item.bar.close);

        // Step 6: Call derived strategy logic
        _derived().on_bar(item.bar);

        // Step 7: Record account snapshot at bar frequency
        // Snapshots are taken every bar for granular equity curves
        // (in production, may skip to reduce memory)
    }

    // === Tick Entry Point ===
    auto x1_tick(const data::Tick& tick) -> void {
        String_View code = tick.code.view();
        latest_price.insert(code.string<A>(), tick.price);

        // Day change detection (coarser than bar-level)
        i64 current_day = tick.time.unix_ns() / (24LL * 3600LL * 1000000000LL);
        if (last_trading_day_ != 0 && current_day != last_trading_day_) {
            _derived().on_dailyclose();
            acc.settle();
            day_counter++;
            _derived().on_dailyopen();
        }
        last_trading_day_ = current_day;

        running_time = tick.time;
        acc.on_price_change(code, tick.price);
        _derived().on_tick(tick);
    }

private:
    // === Internal Helpers ===

    auto _get_latest_price(String_View code) -> Opt<Decimal<8>> {
        auto entry = latest_price.try_get(code);
        if (entry.ok()) return Opt<Decimal<8>>{**entry};
        return {};
    }

    /// Backtest order execution: immediate fill at order price.
    auto _send_order_backtest(Order_Direction dir, Order_Offset offset,
                               Decimal<8> price, f64 volume,
                               String_View code) -> Result<Order, String_View> {
        // Duplicate check
        if (!check_order(dir, offset, code)) {
            return Result<Order, String_View>::err("duplicate_order"_v);
        }

        // Build order
        Order order;
        order.code = code.string<A>();
        order.direction = dir;
        order.offset = offset;
        order.price = price;
        order.volume = volume;
        order.time = running_time;

        // Send to account
        auto result = acc.send_order(order);
        if (!result.ok()) {
            _derived().on_ordererror(dir, offset, price, volume);
            return result;
        }

        // Record bar-level order tracking
        _record_order(code, dir, offset);

        // Immediate fill in backtest mode. `unwrap()` returns a reference
        // (Order is non-copyable so we can't bind by value); pass it through
        // to make_deal and clone the fields we hand back to the caller.
        Order& filled = result.unwrap();
        Trade trade = acc.make_deal(filled);
        _derived().on_deal(trade);

        // Update latest price for this code
        latest_price.insert(code.template string<A>(), price);
        acc.on_price_change(code, price);

        return Result<Order, String_View>::ok(Order{
            filled.order_id.clone(),
            filled.code.clone(),
            filled.direction,
            filled.offset,
            filled.price,
            filled.volume,
            filled.time,
            filled.status});
    }

    /// Sim/live order execution: validate and send, no immediate fill.
    auto _send_order_sim(Order_Direction dir, Order_Offset offset,
                          Decimal<8> price, f64 volume,
                          String_View code) -> Result<Order, String_View> {
        if (!check_order(dir, offset, code)) {
            return Result<Order, String_View>::err("duplicate_order"_v);
        }

        Order order;
        order.code = code.string<A>();
        order.direction = dir;
        order.offset = offset;
        order.price = price;
        order.volume = volume;
        order.time = running_time;

        auto result = acc.send_order(order);
        if (!result.ok()) {
            _derived().on_ordererror(dir, offset, price, volume);
            return result;
        }

        _record_order(code, dir, offset);
        return result;
    }

    /// Record order tracking state for duplicate prevention.
    auto _record_order(String_View code, Order_Direction dir, Order_Offset offset) -> void {
        String<A> code_str = code.string<A>();

        auto code_entry = last_order_towards.try_get(code_str.clone());
        if (!code_entry.ok()) {
            Map<Order_Direction, Order_Offset, A> inner;
            inner.insert(dir, offset);
            last_order_towards.insert(spp::move(code_str), spp::move(inner));
        } else {
            (**code_entry).insert(dir, offset);
        }

        String<A> bo_key = _make_direction_offset_key(dir, offset);
        bar_orders.insert(spp::move(bo_key), bar_id);
    }

    static auto _make_direction_offset_key(Order_Direction dir, Order_Offset offset) -> String<A> {
        char buf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf), "%u_%u",
                       static_cast<u32>(static_cast<u8>(dir)),
                       static_cast<u32>(static_cast<u8>(offset)));
        u64 len = Libc::strlen(buf);
        String<A> result(len);
        result.set_length(len);
        Libc::memcpy(result.data(), buf, len);
        return result;
    }

    /// Flush system_vars into signals at 1-minute boundaries.
    auto _flush_system_vars() -> void {
        // Iterate all system_vars and create signals
        // In Python QA, system_vars are copied to signals then cleared
        // Since Map doesn't support easy iteration with destruct, we use a hack:
        // Track known keys separately or just clear.
        // For simplicity, system_vars serve as a mutable scratchpad for derived classes.
        // The on_1min_bar hook is where derived classes should read and plot them.
        // We don't auto-clear here; derived classes handle that in on_1min_bar.
    }

    static auto _sv_eq(String_View a, String_View b) noexcept -> bool {
        if (a.length() != b.length()) return false;
        return Libc::strncmp(reinterpret_cast<const char*>(a.data()),
                              reinterpret_cast<const char*>(b.data()),
                              a.length()) == 0;
    }

    static auto _bar_id_at_time(Deterministic_Time /*t*/) -> u64 {
        // Simplified: in production this would map time to bar index.
        // For now, return 0 (position entered before start of backtest).
        return 0;
    }
};

} // namespace spp::quant::strategy
