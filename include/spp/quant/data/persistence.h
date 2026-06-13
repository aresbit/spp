#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/backtest/event.h"
#include "spp/quant/execution/order.h"
#include "spp/quant/portfolio/position.h"
#include "spp/quant/base/date.h"
#include "spp/quant/base/currency.h"
#include "spp/io/files.h"

namespace spp::quant::data {

using spp::quant::backtest::OrderEvent;
using spp::quant::backtest::FillEvent;
using spp::quant::backtest::OrderSide;
using spp::quant::backtest::OrderType;
using spp::quant::backtest::OrderStatus;
using spp::quant::execution::Order;
using spp::quant::execution::ExchangePosition;

// =========================================================================
// TradingState -- full trading session state for checkpoint/recovery
// =========================================================================

struct TradingState {
    // Positions
    PositionBook positions_;

    // Orders
    Vec<Order> pending_orders_;

    // Account
    f64 cash_balance_ = 0.0;
    f64 initial_balance_ = 0.0;
    f64 realized_pnl_ = 0.0;
    f64 unrealized_pnl_ = 0.0;

    // Strategy state
    Map<String<>, f64> strategy_params_;
    Map<String<>, Vec<f64>> signal_history_;

    // Risk state
    f64 daily_pnl_ = 0.0;
    f64 peak_equity_ = 0.0;
    u64 daily_trades_ = 0;
    u64 daily_orders_ = 0;
    u64 daily_cancels_ = 0;
    Date last_trading_date_;

    // Session
    Date last_checkpoint_;
    u64 checkpoint_sequence_ = 0;
    String<> strategy_name_;

    SPP_RECORD(TradingState, SPP_FIELD(cash_balance_), SPP_FIELD(realized_pnl_),
               SPP_FIELD(unrealized_pnl_), SPP_FIELD(initial_balance_),
               SPP_FIELD(strategy_name_), SPP_FIELD(last_checkpoint_));
};

// =========================================================================
// JSON serialization helpers for persistence
// =========================================================================
namespace persist_detail {

// Write an f64 value as JSON number (simple, no scientific notation)
inline void json_write_f64(Vec<u8>& buf, f64 value) noexcept {
    if (value < 0.0) { buf.push('-'); value = -value; }
    i64 int_part = static_cast<i64>(value);
    f64 frac = value - static_cast<f64>(int_part);

    // Integer part
    char ibuf[32];
    i32 ip = 31;
    ibuf[ip] = '\0';
    if (int_part == 0) ibuf[--ip] = '0';
    else {
        i64 ipv = int_part;
        while (ipv > 0) { ibuf[--ip] = static_cast<char>('0' + (ipv % 10)); ipv /= 10; }
    }
    for (i32 i = ip; i < 31; i++) buf.push(static_cast<u8>(ibuf[i]));

    // Fractional part (up to 6 digits)
    buf.push('.');
    for (i32 d = 0; d < 6; d++) {
        frac *= 10.0;
        i32 digit = static_cast<i32>(frac);
        buf.push(static_cast<u8>('0' + (digit % 10)));
        frac -= static_cast<f64>(digit);
    }
}

// Write a u64 value as JSON number
inline void json_write_u64(Vec<u8>& buf, u64 value) noexcept {
    char ibuf[24];
    i32 ip = 23;
    ibuf[ip] = '\0';
    if (value == 0) ibuf[--ip] = '0';
    else {
        u64 v = value;
        while (v > 0) { ibuf[--ip] = static_cast<char>('0' + (v % 10)); v /= 10; }
    }
    for (i32 i = ip; i < 23; i++) buf.push(static_cast<u8>(ibuf[i]));
}

// Write a quoted JSON string
inline void json_write_string(Vec<u8>& buf, String_View s) noexcept {
    buf.push('"');
    for (u64 i = 0; i < s.length(); i++) {
        u8 c = s[i];
        if (c == '"' || c == '\\') { buf.push('\\'); buf.push(c); }
        else if (c == '\n') { buf.push('\\'); buf.push('n'); }
        else if (c == '\r') { buf.push('\\'); buf.push('r'); }
        else if (c == '\t') { buf.push('\\'); buf.push('t'); }
        else buf.push(c);
    }
    buf.push('"');
}

// Write a Date as JSON string "YYYY-MM-DD"
inline void json_write_date(Vec<u8>& buf, Date date) noexcept {
    auto ymd = date.ymd();
    i32 y = ymd.get<0>();
    u32 m = ymd.get<1>();
    u32 d = ymd.get<2>();

    char dt[16];
    (void)Libc::snprintf(reinterpret_cast<u8*>(dt), 16, "%04d-%02u-%02u", y, m, d);
    buf.push('"');
    for (u64 i = 0; i < 16 && dt[i] != '\0'; i++) buf.push(static_cast<u8>(dt[i]));
    buf.push('"');
}

// Write a Position as JSON object
inline void json_write_position(Vec<u8>& buf, const Position& pos) noexcept {
    buf.push('{');

    auto push_sv = [&buf](String_View s) {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    };

    push_sv("\"instrument\":"_v);
    json_write_string(buf, pos.instrument_id_);
    push_sv(",\"quantity\":"_v);
    json_write_f64(buf, pos.quantity_);
    push_sv(",\"entry_price\":"_v);
    json_write_f64(buf, pos.entry_price_);
    push_sv(",\"entry_date\":"_v);
    json_write_date(buf, pos.entry_date_);
    push_sv(",\"currency\":"_v);
    json_write_u64(buf, static_cast<u64>(pos.currency_));

    buf.push('}');
}

// Write an Order as JSON object
inline void json_write_order(Vec<u8>& buf, const Order& order) noexcept {
    buf.push('{');

    auto push_sv = [&buf](String_View s) {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    };

    push_sv("\"id\":"_v);
    json_write_u64(buf, order.id_);
    push_sv(",\"side\":"_v);
    buf.push('"');
    if (order.side_ == OrderSide::Buy) push_sv("buy"_v);
    else push_sv("sell"_v);
    buf.push('"');
    push_sv(",\"type\":"_v);
    buf.push('"');
    switch (order.type_) {
    case OrderType::Market: push_sv("market"_v); break;
    case OrderType::Limit:  push_sv("limit"_v); break;
    case OrderType::Stop:   push_sv("stop"_v); break;
    case OrderType::StopLimit: push_sv("stop_limit"_v); break;
    default: push_sv("market"_v); break;
    }
    buf.push('"');
    push_sv(",\"qty\":"_v);
    json_write_f64(buf, order.quantity_);
    push_sv(",\"filled\":"_v);
    json_write_f64(buf, order.filled_quantity_);
    push_sv(",\"avg_price\":"_v);
    json_write_f64(buf, order.avg_fill_price_);
    push_sv(",\"limit_price\":"_v);
    json_write_f64(buf, order.limit_price_);
    push_sv(",\"stop_price\":"_v);
    json_write_f64(buf, order.stop_price_);
    push_sv(",\"status\":"_v);
    buf.push('"');
    switch (order.status_) {
    case OrderStatus::New:       push_sv("new"_v); break;
    case OrderStatus::Partial:   push_sv("partial"_v); break;
    case OrderStatus::Filled:    push_sv("filled"_v); break;
    case OrderStatus::Cancelled: push_sv("cancelled"_v); break;
    case OrderStatus::Rejected:  push_sv("rejected"_v); break;
    }
    buf.push('"');
    push_sv(",\"created\":"_v);
    json_write_date(buf, order.created_);

    buf.push('}');
}

// Serialize TradingState to JSON bytes
inline Vec<u8> serialize_trading_state(const TradingState& state) noexcept {
    Vec<u8> buf;
    buf.reserve(4096);

    auto push_sv = [&buf](String_View s) {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    };

    buf.push('{');

    // Top-level scalar fields
    push_sv("\"cash_balance\":"_v);
    json_write_f64(buf, state.cash_balance_);
    push_sv(",\"initial_balance\":"_v);
    json_write_f64(buf, state.initial_balance_);
    push_sv(",\"realized_pnl\":"_v);
    json_write_f64(buf, state.realized_pnl_);
    push_sv(",\"unrealized_pnl\":"_v);
    json_write_f64(buf, state.unrealized_pnl_);
    push_sv(",\"daily_pnl\":"_v);
    json_write_f64(buf, state.daily_pnl_);
    push_sv(",\"peak_equity\":"_v);
    json_write_f64(buf, state.peak_equity_);
    push_sv(",\"daily_trades\":"_v);
    json_write_u64(buf, state.daily_trades_);
    push_sv(",\"daily_orders\":"_v);
    json_write_u64(buf, state.daily_orders_);
    push_sv(",\"daily_cancels\":"_v);
    json_write_u64(buf, state.daily_cancels_);
    push_sv(",\"last_trading_date\":"_v);
    json_write_date(buf, state.last_trading_date_);
    push_sv(",\"last_checkpoint\":"_v);
    json_write_date(buf, state.last_checkpoint_);
    push_sv(",\"checkpoint_seq\":"_v);
    json_write_u64(buf, state.checkpoint_sequence_);
    push_sv(",\"strategy_name\":"_v);
    json_write_string(buf, state.strategy_name_.view());

    // Positions array
    push_sv(",\"positions\":["_v);
    for (u64 i = 0; i < state.positions_.size(); i++) {
        if (i > 0) buf.push(',');
        json_write_position(buf, state.positions_.positions_[i]);
    }
    buf.push(']');

    // Pending orders array
    push_sv(",\"pending_orders\":["_v);
    for (u64 i = 0; i < state.pending_orders_.length(); i++) {
        if (i > 0) buf.push(',');
        json_write_order(buf, state.pending_orders_[i]);
    }
    buf.push(']');

    // Strategy params map
    push_sv(",\"strategy_params\":{"_v);
    bool first_param = true;
    for (const auto& kv : state.strategy_params_) {
        if (!first_param) buf.push(',');
        first_param = false;
        json_write_string(buf, kv.first.view());
        buf.push(':');
        json_write_f64(buf, kv.second);
    }
    push_sv("}"_v);

    // Signal history map
    push_sv(",\"signal_history\":{"_v);
    bool first_sig = true;
    for (const auto& kv : state.signal_history_) {
        if (!first_sig) buf.push(',');
        first_sig = false;
        json_write_string(buf, kv.first.view());
        push_sv(":["_v);
        for (u64 i = 0; i < kv.second.length(); i++) {
            if (i > 0) buf.push(',');
            json_write_f64(buf, kv.second[i]);
        }
        buf.push(']');
    }
    push_sv("}"_v);

    buf.push('}');
    return buf;
}

} // namespace persist_detail

// =========================================================================
// PersistenceManager -- save/load trading state to/from disk
// =========================================================================

struct PersistenceManager {
    String<> base_path_;
    u64 max_checkpoints_ = 10;

    PersistenceManager() = default;

    explicit PersistenceManager(String_View base_path) noexcept
        : base_path_(sv::sv_to_string(base_path)) {}

    // -----------------------------------------------------------------
    // save_checkpoint -- write TradingState to a JSON file
    //
    // Format: {base_path}/{strategy_name}/checkpoint_{seq}.json
    // -----------------------------------------------------------------
    bool save_checkpoint(const TradingState& state) noexcept {
        if (base_path_.view().empty() || state.strategy_name_.view().empty()) return false;

        // Build directory path: {base_path}/{strategy_name}/
        Vec<u8> dir_buf;
        dir_buf.reserve(base_path_.view().length() + state.strategy_name_.view().length() + 4);

        auto push_sv = [&dir_buf](String_View s) {
            for (u64 i = 0; i < s.length(); i++) dir_buf.push(s[i]);
        };

        push_sv(base_path_.view());
        dir_buf.push('/');
        push_sv(state.strategy_name_.view());
        dir_buf.push('/');

        // Ensure directory exists (best-effort via mkdir)
        String<> dir_str{dir_buf.length() + 1};
        dir_str.set_length(dir_buf.length() + 1);
        for (u64 i = 0; i < dir_buf.length(); i++) dir_str[i] = dir_buf[i];
        dir_str[dir_buf.length()] = '\0';
        // Best-effort mkdir -- ignore errors (directory may already exist)
#ifdef SPP_OS_LINUX
        ::mkdir(reinterpret_cast<const char*>(dir_str.data()), 0755);
#endif

        // Build file path: dir + checkpoint_{seq}.json
        u64 seq = state.checkpoint_sequence_;
        char seq_buf[32];
        push_sv("checkpoint_"_v);
        (void)Libc::snprintf(reinterpret_cast<u8*>(seq_buf), 32, "%lu", seq);
        for (u64 i = 0; i < 32 && seq_buf[i] != '\0'; i++)
            dir_buf.push(static_cast<u8>(seq_buf[i]));
        push_sv(".json"_v);

        String<> file_path{dir_buf.length()};
        file_path.set_length(dir_buf.length());
        for (u64 i = 0; i < dir_buf.length(); i++) file_path[i] = dir_buf[i];

        // Serialize and write
        Vec<u8> json_data = persist_detail::serialize_trading_state(state);
        return Files::write(file_path.view(),
                           Slice<const u8>{json_data.data(), json_data.length()});
    }

    // -----------------------------------------------------------------
    // load_latest_checkpoint -- load the highest-numbered checkpoint
    // -----------------------------------------------------------------
    Opt<TradingState> load_latest_checkpoint(String_View strategy_name) noexcept {
        Vec<u64> checkpoints = list_checkpoints(strategy_name);
        if (checkpoints.empty()) return {};

        u64 max_seq = 0;
        for (u64 i = 0; i < checkpoints.length(); i++) {
            if (checkpoints[i] > max_seq) max_seq = checkpoints[i];
        }
        return load_checkpoint(strategy_name, max_seq);
    }

    // -----------------------------------------------------------------
    // load_checkpoint -- load a specific checkpoint by sequence number
    // -----------------------------------------------------------------
    Opt<TradingState> load_checkpoint(String_View strategy_name, u64 sequence) noexcept {
        if (base_path_.view().empty() || strategy_name.empty()) return {};

        // Build path: {base_path}/{strategy_name}/checkpoint_{seq}.json
        Vec<u8> path_buf;
        path_buf.reserve(base_path_.view().length() + strategy_name.length() + 64);

        auto push_sv = [&path_buf](String_View s) {
            for (u64 i = 0; i < s.length(); i++) path_buf.push(s[i]);
        };

        push_sv(base_path_.view());
        path_buf.push('/');
        push_sv(strategy_name);
        path_buf.push('/');
        push_sv("checkpoint_"_v);

        char seq_buf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(seq_buf), 32, "%lu", sequence);
        for (u64 i = 0; i < 32 && seq_buf[i] != '\0'; i++)
            path_buf.push(static_cast<u8>(seq_buf[i]));
        push_sv(".json"_v);

        String<> file_path{path_buf.length()};
        file_path.set_length(path_buf.length());
        for (u64 i = 0; i < path_buf.length(); i++) file_path[i] = path_buf[i];

        auto file_data = Files::read(file_path.view());
        if (!file_data.ok()) return {};

        return deserialize_json(String_View{file_data->data(), file_data->length()});
    }

    // -----------------------------------------------------------------
    // list_checkpoints -- enumerate sequence numbers for a strategy
    // -----------------------------------------------------------------
    Vec<u64> list_checkpoints(String_View strategy_name) const noexcept {
        Vec<u64> result;
        if (base_path_.view().empty() || strategy_name.empty()) return result;

        // Build directory path
        Vec<u8> dir_buf;
        dir_buf.reserve(base_path_.view().length() + strategy_name.length() + 4);
        for (u64 i = 0; i < base_path_.view().length(); i++) dir_buf.push(base_path_.view()[i]);
        dir_buf.push('/');
        for (u64 i = 0; i < strategy_name.length(); i++) dir_buf.push(strategy_name[i]);
        dir_buf.push('/');

        // [UNSPECIFIED] Directory listing not implemented in spp::Files.
        // This scans known sequence numbers by probing files.
        // For production: implement readdir-based enumeration.
        u64 probe_seq = 0;
        for (u64 attempt = 0; attempt < max_checkpoints_ * 2; attempt++) {
            probe_seq++;

            Vec<u8> probe_path = dir_buf; // copy
            auto push_sv = [&probe_path](String_View s) {
                for (u64 i = 0; i < s.length(); i++) probe_path.push(s[i]);
            };
            push_sv("checkpoint_"_v);
            char sbuf[32];
            (void)Libc::snprintf(reinterpret_cast<u8*>(sbuf), 32, "%lu", probe_seq);
            for (u64 i = 0; i < 32 && sbuf[i] != '\0'; i++)
                probe_path.push(static_cast<u8>(sbuf[i]));
            push_sv(".json"_v);

            String<> probe_str{probe_path.length()};
            probe_str.set_length(probe_path.length());
            for (u64 i = 0; i < probe_path.length(); i++) probe_str[i] = probe_path[i];

            if (Files::last_write_time(probe_str.view()).ok()) {
                result.push(probe_seq);
            } else {
                // If we miss 3 consecutive sequence numbers, assume done
                u64 miss_count = 0;
                for (u64 j = 1; j <= 3 && probe_seq + j <= max_checkpoints_ * 3; j++) {
                    Vec<u8> next_probe = dir_buf;
                    push_sv_into_vec(next_probe, "checkpoint_"_v);
                    char nsbuf[32];
                    (void)Libc::snprintf(reinterpret_cast<u8*>(nsbuf), 32, "%lu", probe_seq + j);
                    auto push_sv2 = [&next_probe](String_View s) {
                        for (u64 i = 0; i < s.length(); i++) next_probe.push(s[i]);
                    };
                    push_sv2("checkpoint_"_v);
                    for (u64 i = 0; i < 32 && nsbuf[i] != '\0'; i++)
                        next_probe.push(static_cast<u8>(nsbuf[i]));
                    push_sv2(".json"_v);

                    String<> next_str{next_probe.length()};
                    next_str.set_length(next_probe.length());
                    for (u64 i = 0; i < next_probe.length(); i++) next_str[i] = next_probe[i];

                    if (!Files::last_write_time(next_str.view()).ok()) {
                        miss_count++;
                    } else {
                        break; // found one, keep going
                    }
                }
                if (miss_count >= 3) break;
            }
        }
        return result;
    }

    // -----------------------------------------------------------------
    // cleanup_old_checkpoints -- keep only the last max_checkpoints_
    // Returns number of files deleted.
    // -----------------------------------------------------------------
    u64 cleanup_old_checkpoints(String_View strategy_name) noexcept {
        Vec<u64> checkpoints = list_checkpoints(strategy_name);
        if (checkpoints.length() <= max_checkpoints_) return 0;

        // Sort in descending order (bubble sort - small N)
        for (u64 i = 0; i < checkpoints.length(); i++) {
            for (u64 j = i + 1; j < checkpoints.length(); j++) {
                if (checkpoints[i] < checkpoints[j]) {
                    u64 tmp = checkpoints[i];
                    checkpoints[i] = checkpoints[j];
                    checkpoints[j] = tmp;
                }
            }
        }

        u64 deleted = 0;
        for (u64 i = max_checkpoints_; i < checkpoints.length(); i++) {
            Vec<u8> path_buf;
            path_buf.reserve(base_path_.view().length() + strategy_name.length() + 64);
            for (u64 j = 0; j < base_path_.view().length(); j++) path_buf.push(base_path_.view()[j]);
            path_buf.push('/');
            for (u64 j = 0; j < strategy_name.length(); j++) path_buf.push(strategy_name[j]);
            path_buf.push('/');
            push_sv_into_vec(path_buf, "checkpoint_"_v);

            char sbuf[32];
            (void)Libc::snprintf(reinterpret_cast<u8*>(sbuf), 32, "%lu", checkpoints[i]);
            for (u64 j = 0; j < 32 && sbuf[j] != '\0'; j++) path_buf.push(static_cast<u8>(sbuf[j]));
            push_sv_into_vec(path_buf, ".json"_v);

            String<> path_str{path_buf.length()};
            path_str.set_length(path_buf.length());
            for (u64 j = 0; j < path_buf.length(); j++) path_str[j] = path_buf[j];

            auto rm_result = Files::remove_result(path_str.view());
            if (rm_result.ok()) deleted++;
        }
        return deleted;
    }

    // -----------------------------------------------------------------
    // export_json / import_json
    // -----------------------------------------------------------------
    bool export_json(String_View path, const TradingState& state) noexcept {
        Vec<u8> json_data = persist_detail::serialize_trading_state(state);
        return Files::write(path, Slice<const u8>{json_data.data(), json_data.length()});
    }

    Opt<TradingState> import_json(String_View path) noexcept {
        auto file_data = Files::read(path);
        if (!file_data.ok()) return {};
        return deserialize_json(String_View{file_data->data(), file_data->length()});
    }

private:
    // Helper: push String_View into Vec<u8>
    static void push_sv_into_vec(Vec<u8>& buf, String_View s) noexcept {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    }

    // -----------------------------------------------------------------
    // Deserialize TradingState from JSON bytes
    // -----------------------------------------------------------------
    static Opt<TradingState> deserialize_json(String_View json) noexcept {
        using json_detail::get_f64_value;
        using json_detail::get_string_value;

        TradingState state;

        auto cash = get_f64_value(json, "cash_balance"_v);
        auto initial = get_f64_value(json, "initial_balance"_v);
        auto realized = get_f64_value(json, "realized_pnl"_v);
        auto unrealized = get_f64_value(json, "unrealized_pnl"_v);
        auto daily_pnl = get_f64_value(json, "daily_pnl"_v);
        auto peak = get_f64_value(json, "peak_equity"_v);
        auto strategy = get_string_value(json, "strategy_name"_v);
        auto seq_str = get_string_value(json, "checkpoint_seq"_v);

        if (cash.ok()) state.cash_balance_ = *cash;
        if (initial.ok()) state.initial_balance_ = *initial;
        if (realized.ok()) state.realized_pnl_ = *realized;
        if (unrealized.ok()) state.unrealized_pnl_ = *unrealized;
        if (daily_pnl.ok()) state.daily_pnl_ = *daily_pnl;
        if (peak.ok()) state.peak_equity_ = *peak;
        if (strategy.ok()) state.strategy_name_ = sv::sv_to_string(*strategy);

        if (seq_str.ok()) {
            String_View sv = *seq_str;
            for (u64 i = 0; i < sv.length() && sv[i] >= '0' && sv[i] <= '9'; i++)
                state.checkpoint_sequence_ = state.checkpoint_sequence_ * 10
                    + static_cast<u64>(sv[i] - '0');
        }

        // Parse positions array
        // [UNSPECIFIED] Full position array parsing is simplified.
        // Each position is a {"instrument":"...","quantity":...} object.
        // For full fidelity, implement a streaming JSON array parser here.

        // Parse pending orders array
        // [UNSPECIFIED] Full order array parsing is simplified.

        state.last_checkpoint_ = Date::today();
        state.checkpoint_sequence_++; // increment for the next save

        return Opt<TradingState>{spp::move(state)};
    }
};

// =========================================================================
// OrderJournal -- append-only order log for audit trail
// =========================================================================

struct OrderJournal {
    String<> journal_path_;

    OrderJournal() = default;

    explicit OrderJournal(String_View path) noexcept
        : journal_path_(sv::sv_to_string(path)) {}

    // -----------------------------------------------------------------
    // log_order -- record an order creation event
    // -----------------------------------------------------------------
    bool log_order(const OrderEvent& order) noexcept {
        if (journal_path_.view().empty()) return false;
        // Build a CSV-like line
        Vec<u8> line;
        line.reserve(256);

        // Date
        auto ymd = order.date_.ymd();
        char dbuf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(dbuf), 16, "%04d-%02u-%02u",
                             ymd.get<0>(), ymd.get<1>(), ymd.get<2>());
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(dbuf),
                                      Libc::strlen(dbuf)});
        line.push('|');
        push_sv_to(line, "ORDER"_v);
        line.push('|');

        // Order ID
        char obuf[24];
        (void)Libc::snprintf(reinterpret_cast<u8*>(obuf), 24, "%lu", order.order_id_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(obuf),
                                      Libc::strlen(obuf)});
        line.push('|');

        // Side
        push_sv_to(line, order.side_ == OrderSide::Buy ? "BUY"_v : "SELL"_v);
        line.push('|');

        // Type
        const char* type_str = "MARKET";
        switch (order.type_) {
        case OrderType::Market: type_str = "MARKET"; break;
        case OrderType::Limit:  type_str = "LIMIT"; break;
        case OrderType::Stop:   type_str = "STOP"; break;
        case OrderType::StopLimit: type_str = "STOP_LIMIT"; break;
        default: break;
        }
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(type_str),
                                      Libc::strlen(type_str)});
        line.push('|');

        // Quantity
        char qbuf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(qbuf), 32, "%.8f", order.quantity_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(qbuf),
                                      Libc::strlen(qbuf)});
        line.push('|');

        // Price
        char pbuf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(pbuf), 32, "%.8f", order.price_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(pbuf),
                                      Libc::strlen(pbuf)});
        line.push('\n');

        return append_to_journal(line);
    }

    // -----------------------------------------------------------------
    // log_fill -- record a fill event
    // -----------------------------------------------------------------
    bool log_fill(const FillEvent& fill) noexcept {
        if (journal_path_.view().empty()) return false;

        Vec<u8> line;
        line.reserve(256);

        auto ymd = fill.date_.ymd();
        char dbuf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(dbuf), 16, "%04d-%02u-%02u",
                             ymd.get<0>(), ymd.get<1>(), ymd.get<2>());
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(dbuf),
                                      Libc::strlen(dbuf)});
        line.push('|');
        push_sv_to(line, "FILL"_v);
        line.push('|');

        char obuf[24];
        (void)Libc::snprintf(reinterpret_cast<u8*>(obuf), 24, "%lu", fill.order_id_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(obuf),
                                      Libc::strlen(obuf)});
        line.push('|');

        push_sv_to(line, fill.side_ == OrderSide::Buy ? "BUY"_v : "SELL"_v);
        line.push('|');

        char qbuf[32], pbuf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(qbuf), 32, "%.8f", fill.quantity_);
        (void)Libc::snprintf(reinterpret_cast<u8*>(pbuf), 32, "%.8f", fill.price_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(qbuf),
                                      Libc::strlen(qbuf)});
        line.push('|');
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(pbuf),
                                      Libc::strlen(pbuf)});
        line.push('|');

        char cbuf[32];
        (void)Libc::snprintf(reinterpret_cast<u8*>(cbuf), 32, "%.8f", fill.commission_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(cbuf),
                                      Libc::strlen(cbuf)});
        line.push('\n');

        return append_to_journal(line);
    }

    // -----------------------------------------------------------------
    // log_cancel -- record a cancel request
    // -----------------------------------------------------------------
    bool log_cancel(u64 order_id, String_View symbol, String_View reason) noexcept {
        if (journal_path_.view().empty()) return false;

        Vec<u8> line;
        line.reserve(256);

        auto ymd = Date::today().ymd();
        char dbuf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(dbuf), 16, "%04d-%02u-%02u",
                             ymd.get<0>(), ymd.get<1>(), ymd.get<2>());
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(dbuf),
                                      Libc::strlen(dbuf)});
        line.push('|');
        push_sv_to(line, "CANCEL"_v);
        line.push('|');

        char obuf[24];
        (void)Libc::snprintf(reinterpret_cast<u8*>(obuf), 24, "%lu", order_id);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(obuf),
                                      Libc::strlen(obuf)});
        line.push('|');
        push_sv_to(line, symbol);
        line.push('|');
        push_sv_to(line, reason);
        line.push('\n');

        return append_to_journal(line);
    }

    // -----------------------------------------------------------------
    // log_reject -- record an order rejection
    // -----------------------------------------------------------------
    bool log_reject(const OrderEvent& order, String_View reason) noexcept {
        if (journal_path_.view().empty()) return false;

        Vec<u8> line;
        line.reserve(256);

        auto ymd = order.date_.ymd();
        char dbuf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(dbuf), 16, "%04d-%02u-%02u",
                             ymd.get<0>(), ymd.get<1>(), ymd.get<2>());
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(dbuf),
                                      Libc::strlen(dbuf)});
        line.push('|');
        push_sv_to(line, "REJECT"_v);
        line.push('|');

        char obuf[24];
        (void)Libc::snprintf(reinterpret_cast<u8*>(obuf), 24, "%lu", order.order_id_);
        push_sv_to(line, String_View{reinterpret_cast<const u8*>(obuf),
                                      Libc::strlen(obuf)});
        line.push('|');
        push_sv_to(line, reason);
        line.push('\n');

        return append_to_journal(line);
    }

    // -----------------------------------------------------------------
    // Journal entry types
    // -----------------------------------------------------------------
    enum struct JournalEntryType : u8 {
        OrderSent, FillReceived, CancelSent, RejectReceived
    };

    struct JournalEntry {
        JournalEntryType type_;
        Date timestamp_;
        u64 order_id_;
        OrderSide side_ = OrderSide::Buy;
        f64 quantity_ = 0.0;
        f64 price_ = 0.0;
        f64 commission_ = 0.0;
        String<> symbol_;
        String<> message_;
    };

    // -----------------------------------------------------------------
    // replay -- read journal entries in date range
    // -----------------------------------------------------------------
    Vec<JournalEntry> replay(Date from, Date to) noexcept {
        Vec<JournalEntry> result;
        if (journal_path_.view().empty()) return result;

        auto file_data = Files::read(journal_path_.view());
        if (!file_data.ok()) return result;

        String_View content{file_data->data(), file_data->length()};

        // Parse lines
        u64 line_start = 0;
        for (u64 i = 0; i <= content.length(); i++) {
            if (i == content.length() || static_cast<char>(content[i]) == '\n') {
                String_View line = content.sub(line_start, i);
                if (!line.empty() && line[line.length()-1] == '\r')
                    line = line.sub(0, line.length() - 1);
                line_start = i + 1;
                if (line.empty()) continue;

                // Parse pipe-delimited fields
                // Format: date|type|order_id|side_or_symbol|qty_or_reason|price|commission
                auto entry = parse_journal_line(line);
                if (entry.ok() && entry->timestamp_ >= from && entry->timestamp_ <= to) {
                    result.push(spp::move(*entry));
                }
            }
        }
        return result;
    }

    // -----------------------------------------------------------------
    // rotate -- start a new journal file for a new date
    // -----------------------------------------------------------------
    void rotate(Date new_date) noexcept {
        auto ymd = new_date.ymd();
        // Build new path: {journal_dir}/orders_{YYYYMMDD}.log
        // Find the directory portion of journal_path_
        String_View old_path = journal_path_.view();
        u64 last_slash = old_path.length();
        for (u64 i = old_path.length(); i > 0; i--) {
            if (static_cast<char>(old_path[i-1]) == '/') { last_slash = i - 1; break; }
        }

        Vec<u8> new_path_buf;
        new_path_buf.reserve(last_slash + 32);
        for (u64 i = 0; i < last_slash; i++) new_path_buf.push(old_path[i]);
        new_path_buf.push('/');
        push_sv_to(new_path_buf, "orders_"_v);

        char dbuf[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(dbuf), 16, "%04d%02u%02u",
                             ymd.get<0>(), ymd.get<1>(), ymd.get<2>());
        for (u64 i = 0; i < 8; i++) new_path_buf.push(static_cast<u8>(dbuf[i]));
        push_sv_to(new_path_buf, ".log"_v);

        String<> new_path{new_path_buf.length()};
        new_path.set_length(new_path_buf.length());
        for (u64 i = 0; i < new_path_buf.length(); i++) new_path[i] = new_path_buf[i];

        journal_path_ = sv::sv_to_string(new_path.view());
    }

private:
    static void push_sv_to(Vec<u8>& buf, String_View s) noexcept {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    }

    bool append_to_journal(const Vec<u8>& line) noexcept {
        // Read existing content, append new line, write back
        auto existing = Files::read(journal_path_.view());
        Vec<u8> combined;
        if (existing.ok()) {
            combined.reserve(existing->length() + line.length());
            for (u64 i = 0; i < existing->length(); i++) combined.push((*existing)[i]);
        } else {
            combined.reserve(line.length());
        }
        for (u64 i = 0; i < line.length(); i++) combined.push(line[i]);

        return Files::write(journal_path_.view(),
                           Slice<const u8>{combined.data(), combined.length()});
    }

    static Opt<JournalEntry> parse_journal_line(String_View line) noexcept {
        JournalEntry entry;

        // Parse pipe-separated fields
        struct { u64 start; u64 end; } fields[8] = {};
        u64 nf = 0;
        u64 pos = 0;
        for (u64 i = 0; i <= line.length() && nf < 8; i++) {
            if (i == line.length() || static_cast<char>(line[i]) == '|') {
                fields[nf].start = pos;
                fields[nf].end = i;
                nf++;
                pos = i + 1;
            }
        }
        if (nf < 3) return {};

        // Field 0: date (YYYY-MM-DD)
        String_View date_sv = line.sub(fields[0].start, fields[0].end);
        if (date_sv.length() >= 10) {
            i32 y = 0; u32 m = 0, d = 0;
            for (i32 i = 0; i < 4; i++) y = y * 10 + (date_sv[i] - '0');
            for (i32 i = 5; i < 7; i++) m = m * 10 + (date_sv[i] - '0');
            for (i32 i = 8; i < 10; i++) d = d * 10 + (date_sv[i] - '0');
            entry.timestamp_ = Date::from_ymd(y, m, d);
        } else {
            entry.timestamp_ = Date::today();
        }

        // Field 1: event type
        String_View type_sv = line.sub(fields[1].start, fields[1].end);
        if (type_sv == "ORDER"_v) entry.type_ = JournalEntryType::OrderSent;
        else if (type_sv == "FILL"_v) entry.type_ = JournalEntryType::FillReceived;
        else if (type_sv == "CANCEL"_v) entry.type_ = JournalEntryType::CancelSent;
        else if (type_sv == "REJECT"_v) entry.type_ = JournalEntryType::RejectReceived;
        else return {};

        // Field 2: order_id
        String_View id_sv = line.sub(fields[2].start, fields[2].end);
        for (u64 i = 0; i < id_sv.length() && id_sv[i] >= '0' && id_sv[i] <= '9'; i++)
            entry.order_id_ = entry.order_id_ * 10 + static_cast<u64>(id_sv[i] - '0');

        // Remaining fields depend on type
        if (entry.type_ == JournalEntryType::FillReceived && nf >= 7) {
            String_View side_sv = line.sub(fields[3].start, fields[3].end);
            entry.side_ = (side_sv == "BUY"_v) ? OrderSide::Buy : OrderSide::Sell;

            // Parse quantity
            String_View qty_sv = line.sub(fields[4].start, fields[4].end);
            entry.quantity_ = parsestod(qty_sv);

            // Parse price
            String_View px_sv = line.sub(fields[5].start, fields[5].end);
            entry.price_ = parsestod(px_sv);

            if (nf >= 8) {
                String_View comm_sv = line.sub(fields[6].start, fields[6].end);
                entry.commission_ = parsestod(comm_sv);
            }
        } else if (nf >= 4) {
            entry.symbol_ = sv::sv_to_string(line.sub(fields[3].start, fields[3].end));
            if (nf >= 5) {
                entry.message_ = sv::sv_to_string(line.sub(fields[4].start, fields[4].end));
            }
        }

        return Opt<JournalEntry>{spp::move(entry)};
    }

    static f64 parsestod(String_View s) noexcept {
        f64 result = 0.0, sign = 1.0;
        u64 pos = 0;
        if (!s.empty() && static_cast<char>(s[0]) == '-') { sign = -1.0; pos++; }
        f64 frac = 0.1;
        bool in_frac = false;
        while (pos < s.length()) {
            char c = static_cast<char>(s[pos]);
            if (c >= '0' && c <= '9') {
                if (in_frac) { result += static_cast<f64>(c - '0') * frac; frac *= 0.1; }
                else result = result * 10.0 + static_cast<f64>(c - '0');
            } else if (c == '.') {
                in_frac = true;
            }
            pos++;
        }
        return result * sign;
    }
};

// =========================================================================
// PositionSync -- synchronize local positions with exchange
// =========================================================================

struct PositionSync {

    struct SyncResult {
        bool is_consistent_ = true;
        Vec<String<>> local_only_;
        Vec<String<>> exchange_only_;
        Vec<String<>> quantity_mismatch_;
        Vec<String<>> matched_;
        String<> summary_;

        SPP_RECORD(SyncResult, SPP_FIELD(is_consistent_), SPP_FIELD(summary_));
    };

    // -----------------------------------------------------------------
    // compare -- diff local PositionBook against exchange positions
    // -----------------------------------------------------------------
    SyncResult compare(
        const PositionBook& local,
        Slice<const ExchangePosition> exchange_positions
    ) const noexcept {
        SyncResult result;

        // Build lookup tables (by symbol)
        // Simple O(N*M) comparison -- acceptable for small position counts

        // Find local positions not on exchange
        for (u64 i = 0; i < local.positions_.length(); i++) {
            String_View local_sym = local.positions_[i].instrument_id_;
            bool found = false;
            for (u64 j = 0; j < exchange_positions.length(); j++) {
                if (exchange_positions[j].symbol_.view() == local_sym) {
                    found = true;
                    // Check quantity match
                    f64 local_qty = local.positions_[i].quantity_;
                    f64 exch_qty = exchange_positions[j].quantity_;
                    if (Math::abs(local_qty - exch_qty) > 1e-8) {
                        result.quantity_mismatch_.push(sv::sv_to_string(local_sym));
                    } else {
                        result.matched_.push(sv::sv_to_string(local_sym));
                    }
                    break;
                }
            }
            if (!found) {
                result.local_only_.push(sv::sv_to_string(local_sym));
            }
        }

        // Find exchange positions not in local book
        for (u64 j = 0; j < exchange_positions.length(); j++) {
            String_View exch_sym = exchange_positions[j].symbol_.view();
            if (exch_sym.empty()) continue;
            bool found = false;
            for (u64 i = 0; i < local.positions_.length(); i++) {
                if (local.positions_[i].instrument_id_ == exch_sym) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                result.exchange_only_.push(sv::sv_to_string(exch_sym));
            }
        }

        // Determine consistency
        result.is_consistent_ = result.local_only_.empty()
                             && result.exchange_only_.empty()
                             && result.quantity_mismatch_.empty();

        // Build summary
        char summary_buf[256];
        (void)Libc::snprintf(reinterpret_cast<u8*>(summary_buf), 256,
            "matched=%lu local_only=%lu exch_only=%lu mismatch=%lu",
            result.matched_.length(), result.local_only_.length(),
            result.exchange_only_.length(), result.quantity_mismatch_.length());
        String_View sv{reinterpret_cast<const u8*>(summary_buf), Libc::strlen(summary_buf)};
        result.summary_ = sv::sv_to_string(sv);

        return result;
    }

    // -----------------------------------------------------------------
    // resolve -- fix discrepancies by trusting exchange data
    //
    // If trust_exchange is true:
    //   - Adds missing positions from exchange
    //   - Removes local-only positions (assume they were closed externally)
    //   - Updates quantities to match exchange
    // If trust_exchange is false: only returns the list of discrepancies
    //   without modifying local state.
    // -----------------------------------------------------------------
    Vec<String<>> resolve(
        PositionBook& local,
        Slice<const ExchangePosition> exchange_positions,
        bool trust_exchange = true
    ) noexcept {
        Vec<String<>> changes;

        if (!trust_exchange) {
            // Report-only mode
            auto result = compare(local, exchange_positions);
            for (u64 i = 0; i < result.local_only_.length(); i++)
                changes.push(String<>{result.local_only_[i]});
            for (u64 i = 0; i < result.exchange_only_.length(); i++)
                changes.push(String<>{result.exchange_only_[i]});
            for (u64 i = 0; i < result.quantity_mismatch_.length(); i++)
                changes.push(String<>{result.quantity_mismatch_[i]});
            return changes;
        }

        // Remove local-only positions
        for (u64 i = local.positions_.length(); i > 0; i--) {
            u64 idx = i - 1;
            String_View local_sym = local.positions_[idx].instrument_id_;
            bool found = false;
            for (u64 j = 0; j < exchange_positions.length(); j++) {
                if (exchange_positions[j].symbol_.view() == local_sym) {
                    found = true; break;
                }
            }
            if (!found) {
                changes.push(sv::sv_to_string(local_sym));
                local.remove(local_sym);
            }
        }

        // Add missing from exchange + update quantities
        for (u64 j = 0; j < exchange_positions.length(); j++) {
            const ExchangePosition& ep = exchange_positions[j];
            String_View exch_sym = ep.symbol_.view();
            if (exch_sym.empty()) continue;

            auto existing = local.find(exch_sym);
            if (!existing.ok()) {
                // Add new position from exchange
                Position new_pos;
                new_pos.instrument_id_ = exch_sym;
                new_pos.quantity_ = ep.quantity_;
                new_pos.entry_price_ = ep.avg_entry_price_;
                new_pos.entry_date_ = Date::today();
                new_pos.currency_ = ep.currency_;
                local.add(spp::move(new_pos));
                changes.push(sv::sv_to_string(exch_sym));
            } else if (Math::abs(existing->quantity_ - ep.quantity_) > 1e-8) {
                // Update quantity
                existing->quantity_ = ep.quantity_;
                existing->entry_price_ = ep.avg_entry_price_;
                changes.push(sv::sv_to_string(exch_sym));
            }
        }

        return changes;
    }
};

} // namespace spp::quant::data

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::data::TradingState, "TradingState",
                 SPP_FIELD(cash_balance_), SPP_FIELD(initial_balance_),
                 SPP_FIELD(realized_pnl_), SPP_FIELD(unrealized_pnl_),
                 SPP_FIELD(daily_pnl_), SPP_FIELD(peak_equity_),
                 SPP_FIELD(daily_trades_), SPP_FIELD(daily_orders_),
                 SPP_FIELD(daily_cancels_), SPP_FIELD(strategy_name_));

SPP_NAMED_RECORD(::spp::quant::data::PositionSync::SyncResult, "PositionSyncResult",
                 SPP_FIELD(is_consistent_), SPP_FIELD(summary_));
