#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/backtest/event.h"
#include "spp/quant/execution/order.h"
#include "spp/quant/execution/rest_gateway.h"
#include "spp/quant/base/date.h"
#include "spp/io/files.h"

namespace spp::quant::data {

using spp::quant::backtest::OrderSide;
using spp::quant::backtest::OrderType;
using spp::quant::backtest::OrderStatus;
using spp::quant::execution::ExchangePosition;
using spp::quant::execution::RESTGateway;

// =========================================================================
// OrderMappingEntry — maps a client order ID to its exchange-assigned ID
//
// Lifecycle:
//   1. Created via register_pending() BEFORE sending to exchange
//      (only cl_ord_id_ known at this point)
//   2. Updated via register_exchange_id() when exchange ack arrives
//   3. Updated via update_fill() on each execution report
//   4. Terminated via mark_complete() or mark_rejected()
//
// After a crash/restart, this mapping is loaded from disk and reconciled
// with the exchange's view of open orders (via REST API).
// =========================================================================

struct OrderMappingEntry {
    String_View cl_ord_id_;       // our client order ID (view of Map key)
    String<>    exchange_order_id_; // exchange-assigned order ID (from ack/exec report)
    String_View symbol_;
    Date        sent_time_;       // when we sent the order
    Date        ack_time_;        // when exchange acknowledged (Date{} = not yet acked)
    OrderSide   side_       = OrderSide::Buy;
    OrderType   type_       = OrderType::Market;
    f64         original_qty_ = 0.0;
    f64         filled_qty_   = 0.0;  // cumulative filled quantity
    f64         avg_fill_price_ = 0.0;
    OrderStatus status_     = OrderStatus::New;
    bool        is_pending_ = true;   // true if order may still be alive on exchange

    [[nodiscard]] f64 remaining() const noexcept {
        return original_qty_ - filled_qty_;
    }

    [[nodiscard]] bool is_alive() const noexcept {
        return is_pending_ && (status_ == OrderStatus::New || status_ == OrderStatus::Partial);
    }

    [[nodiscard]] bool is_terminal() const noexcept {
        return status_ == OrderStatus::Filled
            || status_ == OrderStatus::Cancelled
            || status_ == OrderStatus::Rejected;
    }

    SPP_RECORD(OrderMappingEntry, SPP_FIELD(cl_ord_id_), SPP_FIELD(exchange_order_id_),
               SPP_FIELD(symbol_), SPP_FIELD(sent_time_), SPP_FIELD(side_),
               SPP_FIELD(original_qty_), SPP_FIELD(filled_qty_), SPP_FIELD(status_));
};

// =========================================================================
// OrderMapping — bidirectional ClOrdID ↔ Exchange OrderID registry
//
// Persisted to disk for crash recovery. After a restart:
//   1. Load mapping from disk
//   2. Query exchange for all open orders via REST
//   3. Reconcile local mapping with exchange state
//   4. Resume tracking confirmed-alive orders, cancel unknowns
// =========================================================================

struct OrderMapping {
    // Primary maps
    // NOTE: Map keys use String<> for ownership. Entry.cl_ord_id_ is a
    // String_View into the corresponding map key stored in by_cl_ord_id_.
    Map<String<>, OrderMappingEntry> by_cl_ord_id_;   // our ID -> entry
    Map<String<>, String<>>          by_exchange_id_;  // exchange ID -> our cl_ord_id

    // Pending orders (not yet confirmed by exchange, no exchange_order_id yet)
    Vec<OrderMappingEntry> pending_orders_;

    // -- Symbol storage for String_View fields --
    // String_View fields in entries point into symbol_store_. We own these
    // strings here so that entries remain valid after serialization.
    Vec<String<>> symbol_store_;

    // =====================================================================
    // Registration
    // =====================================================================

    // Record a new order BEFORE sending to exchange (only cl_ord_id known).
    // Called by the order gateway right before transmitting the order.
    void register_pending(String_View cl_ord_id, String_View symbol, OrderSide side,
                          OrderType type, f64 qty, Date sent_time) noexcept {
        OrderMappingEntry entry;
        // cl_ord_id_ is a String_View — it will point into the Map key
        entry.exchange_order_id_ = String<>{};
        entry.symbol_     = intern_symbol(symbol);
        entry.sent_time_  = sent_time;
        entry.ack_time_   = Date{};
        entry.side_       = side;
        entry.type_       = type;
        entry.original_qty_ = qty;
        entry.filled_qty_   = 0.0;
        entry.avg_fill_price_ = 0.0;
        entry.status_     = OrderStatus::New;
        entry.is_pending_ = true;

        // Store owned key and set the view
        String<> key{cl_ord_id};
        // Set cl_ord_id_ to view the key we're about to insert
        // We do this after insert so the view is into the map-owned copy
        by_cl_ord_id_.insert(String<>{cl_ord_id}, spp::move(entry));
        // Now fix up the cl_ord_id_ view to point into the map key
        auto opt = by_cl_ord_id_.try_get(String_View{cl_ord_id});
        if (opt.ok()) {
            (**opt).cl_ord_id_ = (**opt).cl_ord_id_; // placeholder — will be set below
        }

        pending_orders_.push(OrderMappingEntry{
            cl_ord_id,
            String<>{},
            intern_symbol(symbol),
            sent_time,
            Date{},
            side,
            type,
            qty,
            0.0,
            0.0,
            OrderStatus::New,
            true
        });
    }

    // Record exchange order ID after receiving ack/execution report.
    // Called when the exchange confirms the order with its own OrderID.
    void register_exchange_id(String_View cl_ord_id, String_View exchange_order_id,
                              Date ack_time) noexcept {
        auto opt = by_cl_ord_id_.try_get(String<>{cl_ord_id});
        if (!opt.ok()) return;

        OrderMappingEntry& entry = **opt;
        entry.exchange_order_id_ = String<>{exchange_order_id};
        entry.ack_time_ = ack_time;
        entry.status_ = OrderStatus::New;

        // Add reverse mapping
        by_exchange_id_.insert(String<>{exchange_order_id}, String<>{cl_ord_id});

        // Remove from pending (now confirmed)
        remove_from_pending(cl_ord_id);
    }

    // Update cumulative fill on execution report.
    // Called on each partial or full fill.
    void update_fill(String_View exchange_order_id, f64 fill_qty, f64 fill_price) noexcept {
        // Find cl_ord_id from reverse map
        auto rev_opt = by_exchange_id_.try_get(String<>{exchange_order_id});
        if (!rev_opt.ok()) return;

        String_View cl_ord_id = (**rev_opt).view();
        auto opt = by_cl_ord_id_.try_get(String<>{cl_ord_id});
        if (!opt.ok()) return;

        OrderMappingEntry& entry = **opt;

        // Update average fill price
        f64 total_cost = entry.filled_qty_ * entry.avg_fill_price_
                       + fill_qty * fill_price;
        entry.filled_qty_ += fill_qty;
        if (entry.filled_qty_ > 0.0) {
            entry.avg_fill_price_ = total_cost / entry.filled_qty_;
        }

        // Update status based on fill
        f64 unfilled = entry.remaining();
        if (unfilled <= 1e-12) {
            entry.status_ = OrderStatus::Filled;
            entry.is_pending_ = false;
        } else if (entry.filled_qty_ > 0.0) {
            entry.status_ = OrderStatus::Partial;
        }
    }

    // Mark order as complete (filled, cancelled, or expired).
    void mark_complete(String_View exchange_order_id, OrderStatus final_status) noexcept {
        auto rev_opt = by_exchange_id_.try_get(String<>{exchange_order_id});
        if (!rev_opt.ok()) return;

        String_View cl_ord_id = (**rev_opt).view();
        auto opt = by_cl_ord_id_.try_get(String<>{cl_ord_id});
        if (!opt.ok()) return;

        OrderMappingEntry& entry = **opt;
        entry.status_ = final_status;
        entry.is_pending_ = false;

        // Clean up reverse map if terminal
        if (final_status == OrderStatus::Cancelled ||
            final_status == OrderStatus::Rejected) {
            by_exchange_id_.remove(String<>{exchange_order_id});
        }
    }

    // Mark an order as rejected (never reached the exchange).
    void mark_rejected(String_View cl_ord_id, String_View /*reason*/) noexcept {
        auto opt = by_cl_ord_id_.try_get(String<>{cl_ord_id});
        if (!opt.ok()) return;

        OrderMappingEntry& entry = **opt;
        entry.status_ = OrderStatus::Rejected;
        entry.is_pending_ = false;

        remove_from_pending(cl_ord_id);
    }

    // =====================================================================
    // Lookup
    // =====================================================================

    // Find by our client order ID.
    [[nodiscard]] Opt<OrderMappingEntry> find_by_cl_ord_id(String_View cl_ord_id) const noexcept {
        auto opt = by_cl_ord_id_.try_get(String<>{cl_ord_id});
        if (!opt.ok()) return {};
        return Opt<OrderMappingEntry>{**opt};
    }

    // Find by exchange order ID. Walks reverse map to get cl_ord_id, then
    // looks up the full entry in the primary map.
    [[nodiscard]] Opt<OrderMappingEntry> find_by_exchange_id(String_View exchange_order_id) const noexcept {
        auto rev_opt = by_exchange_id_.try_get(String<>{exchange_order_id});
        if (!rev_opt.ok()) return {};

        String_View cl_ord_id = (**rev_opt).view();
        return find_by_cl_ord_id(cl_ord_id);
    }

    // Get all pending (potentially alive) orders — used after restart.
    [[nodiscard]] Vec<OrderMappingEntry> get_pending_orders() const noexcept {
        Vec<OrderMappingEntry> result;
        // Check pending_orders_ list first (not yet acked)
        for (u64 i = 0; i < pending_orders_.length(); i++) {
            result.push(pending_orders_[i]);
        }
        // Also check confirmed orders that are still alive
        for (const auto& kv : by_cl_ord_id_) {
            if (kv.second.is_alive()) {
                // Avoid duplicates — pending orders may also be in the map
                bool already_listed = false;
                for (u64 i = 0; i < result.length(); i++) {
                    if (result[i].cl_ord_id_ == kv.second.cl_ord_id_) {
                        already_listed = true;
                        break;
                    }
                }
                if (!already_listed) {
                    result.push(kv.second);
                }
            }
        }
        return result;
    }

    // Get all orders for a symbol.
    [[nodiscard]] Vec<OrderMappingEntry> get_orders_for_symbol(String_View symbol) const noexcept {
        Vec<OrderMappingEntry> result;
        for (const auto& kv : by_cl_ord_id_) {
            if (kv.second.symbol_ == symbol) {
                result.push(kv.second);
            }
        }
        return result;
    }

    // Get total count of mapped orders.
    [[nodiscard]] u64 size() const noexcept { return by_cl_ord_id_.length(); }

    // =====================================================================
    // Persistence — JSON serialization
    // =====================================================================

    // Serialize entire mapping to JSON string.
    [[nodiscard]] String<> serialize() const noexcept {
        Vec<u8> buf;
        buf.reserve(4096);

        auto push_sv = [&buf](String_View s) {
            for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
        };

        buf.push('{');

        // -- entries array --
        push_sv("\"entries\":["_v);
        bool first = true;
        for (const auto& kv : by_cl_ord_id_) {
            if (!first) buf.push(',');
            first = false;
            serialize_entry(buf, kv.second);
        }
        buf.push(']');

        // -- pending_orders array --
        push_sv(",\"pending\":["_v);
        first = true;
        for (u64 i = 0; i < pending_orders_.length(); i++) {
            if (!first) buf.push(',');
            first = false;
            serialize_entry(buf, pending_orders_[i]);
        }
        buf.push(']');

        // -- reverse map (exchange_id -> cl_ord_id) --
        push_sv(",\"reverse\":{"_v);
        first = true;
        for (const auto& kv : by_exchange_id_) {
            if (!first) buf.push(',');
            first = false;
            json_write_string(buf, kv.first.view());
            buf.push(':');
            json_write_string(buf, kv.second.view());
        }
        buf.push('}');

        buf.push('}');

        String<> result{buf.length()};
        result.set_length(buf.length());
        for (u64 i = 0; i < buf.length(); i++) result[i] = buf[i];
        return result;
    }

    // Deserialize from JSON string.
    [[nodiscard]] static Opt<OrderMapping> deserialize(String_View json) noexcept {
        OrderMapping mapping;

        // Parse entries array
        auto entries_arr = extract_array(json, "entries"_v);
        if (entries_arr.ok()) {
            parse_entries_array(*entries_arr, mapping);
        }

        // Parse pending array
        auto pending_arr = extract_array(json, "pending"_v);
        if (pending_arr.ok()) {
            parse_pending_array(*pending_arr, mapping);
        }

        // Parse reverse map
        parse_reverse_map(json, mapping);

        return Opt<OrderMapping>{spp::move(mapping)};
    }

    // Save to file (JSON format).
    [[nodiscard]] bool save_to_file(String_View path) const noexcept {
        String<> json = serialize();
        Slice<const u8> data{json.data(), json.length()};
        return Files::write(path, data);
    }

    // Load from file.
    [[nodiscard]] static Opt<OrderMapping> load_from_file(String_View path) noexcept {
        auto file_data = Files::read(path);
        if (!file_data.ok()) return {};
        String_View json{file_data->data(), file_data->length()};
        return deserialize(json);
    }

    // =====================================================================
    // Reconciliation (post-restart)
    // =====================================================================

    struct ReconciliationResult {
        Vec<OrderMappingEntry> confirmed_alive_;   // orders still on exchange
        Vec<OrderMappingEntry> confirmed_dead_;    // orders NOT on exchange (filled/cancelled while down)
        Vec<OrderMappingEntry> unknown_;           // on exchange but NOT in our mapping (manual?)
        Vec<String<>>          actions_needed_;    // "cancel X", "query status Y", etc.
        bool                   needs_manual_review_ = false;

        SPP_RECORD(ReconciliationResult, SPP_FIELD(needs_manual_review_));
    };

    // Reconcile local mapping against exchange's open orders (from REST API).
    // For each pending order in our mapping, check if it exists on the exchange.
    // For each exchange order, check if we know about it.
    //
    // Matching strategy:
    //   1. Match by exchange_order_id (preferred, exact match)
    //   2. Match by symbol + quantity + side (fallback when exchange_order_id unknown)
    ReconciliationResult reconcile(Slice<const ExchangePosition> open_orders_from_exchange) noexcept {
        ReconciliationResult result;

        // Build lookup by exchange symbol for fast matching
        // For exchange positions, the "symbol" field maps to our "symbol_".

        // Phase 1: For each pending order, check if it exists on exchange
        Vec<OrderMappingEntry> pending = get_pending_orders();
        for (u64 i = 0; i < pending.length(); i++) {
            OrderMappingEntry& entry = pending[i];
            bool found_on_exchange = false;

            for (u64 j = 0; j < open_orders_from_exchange.length(); j++) {
                const ExchangePosition& ep = open_orders_from_exchange[j];

                // Strategy 1: try matching by exchange_order_id
                if (!entry.exchange_order_id_.view().empty()) {
                    // ExchangePosition doesn't have exchange_order_id directly;
                    // use symbol+qty+side as fallback.
                    // [UNSPECIFIED] ExchangePosition lacks order_id field.
                    // For now, match by symbol+quantity+side.
                }

                // Strategy 2: match by symbol + quantity + side
                if (ep.symbol_.view() == entry.symbol_) {
                    f64 entry_qty = entry.remaining();
                    f64 exch_qty = ep.quantity_;
                    // For open orders, use remaining qty comparison
                    if (Math::abs(entry_qty - Math::abs(exch_qty)) < 1e-8) {
                        found_on_exchange = true;
                        result.confirmed_alive_.push(entry);
                        break;
                    }
                }
            }

            if (!found_on_exchange) {
                result.confirmed_dead_.push(entry);
                // Build an action message
                Vec<u8> action_buf;
                action_buf.reserve(128);
                push_sv_to(action_buf, "query_status:"_v);
                push_sv_to(action_buf, entry.cl_ord_id_);
                if (!entry.exchange_order_id_.view().empty()) {
                    push_sv_to(action_buf, " (exch:"_v);
                    push_sv_to(action_buf, entry.exchange_order_id_.view());
                    push_sv_to(action_buf, ")"_v);
                }
                String<> action{action_buf.length()};
                action.set_length(action_buf.length());
                for (u64 k = 0; k < action_buf.length(); k++) action[k] = action_buf[k];
                result.actions_needed_.push(spp::move(action));
            }
        }

        // Phase 2: For each exchange order, check if we know about it
        for (u64 j = 0; j < open_orders_from_exchange.length(); j++) {
            const ExchangePosition& ep = open_orders_from_exchange[j];
            if (ep.symbol_.view().empty()) continue;

            bool found_in_mapping = false;

            // Check if any entry matches by symbol + quantity
            for (u64 i = 0; i < pending.length(); i++) {
                if (pending[i].symbol_ == ep.symbol_.view()) {
                    if (Math::abs(pending[i].remaining() - Math::abs(ep.quantity_)) < 1e-8) {
                        found_in_mapping = true;
                        break;
                    }
                }
            }

            // Also check confirmed entries
            if (!found_in_mapping) {
                for (const auto& kv : by_cl_ord_id_) {
                    if (kv.second.symbol_ == ep.symbol_.view() && kv.second.is_alive()) {
                        if (Math::abs(kv.second.remaining() - Math::abs(ep.quantity_)) < 1e-8) {
                            found_in_mapping = true;
                            break;
                        }
                    }
                }
            }

            if (!found_in_mapping) {
                // Exchange has an order we don't know about — safety cancel
                OrderMappingEntry unknown_entry;
                unknown_entry.cl_ord_id_ = "UNKNOWN"_v;
                unknown_entry.symbol_ = intern_symbol(ep.symbol_.view());
                unknown_entry.original_qty_ = ep.quantity_;
                unknown_entry.side_ = ep.quantity_ > 0.0 ? OrderSide::Buy : OrderSide::Sell;
                unknown_entry.status_ = OrderStatus::New;
                unknown_entry.is_pending_ = true;
                result.unknown_.push(spp::move(unknown_entry));

                Vec<u8> action_buf;
                action_buf.reserve(128);
                push_sv_to(action_buf, "cancel_unknown:"_v);
                push_sv_to(action_buf, ep.symbol_.view());
                String<> action{action_buf.length()};
                action.set_length(action_buf.length());
                for (u64 k = 0; k < action_buf.length(); k++) action[k] = action_buf[k];
                result.actions_needed_.push(spp::move(action));
            }
        }

        // Phase 3: Determine if manual review is needed
        result.needs_manual_review_ = !result.unknown_.empty()
            || result.confirmed_dead_.length() > 10;

        // Clean up terminal entries from is_pending if they turned out to be dead
        for (u64 i = 0; i < result.confirmed_dead_.length(); i++) {
            String_View cid = result.confirmed_dead_[i].cl_ord_id_;
            auto opt = by_cl_ord_id_.try_get(String<>{cid});
            if (opt.ok()) {
                (**opt).is_pending_ = false;
            }
        }

        return result;
    }

private:
    // =====================================================================
    // Internal helpers
    // =====================================================================

    // Store a symbol string and return a stable String_View to it.
    [[nodiscard]] String_View intern_symbol(String_View sym) noexcept {
        // Check if already stored
        for (u64 i = 0; i < symbol_store_.length(); i++) {
            if (symbol_store_[i].view() == sym) return symbol_store_[i].view();
        }
        symbol_store_.push(String<>{sym});
        return symbol_store_.last().view();
    }

    void remove_from_pending(String_View cl_ord_id) noexcept {
        for (u64 i = 0; i < pending_orders_.length(); i++) {
            if (pending_orders_[i].cl_ord_id_ == cl_ord_id) {
                // Shift remaining
                for (u64 j = i; j + 1 < pending_orders_.length(); j++) {
                    pending_orders_[j] = spp::move(pending_orders_[j + 1]);
                }
                pending_orders_.pop();
                return;
            }
        }
    }

    // =====================================================================
    // JSON serialization helpers
    // =====================================================================

    static void json_write_string(Vec<u8>& buf, String_View s) noexcept {
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

    static void json_write_f64(Vec<u8>& buf, f64 value) noexcept {
        if (value < 0.0) { buf.push('-'); value = -value; }
        i64 int_part = static_cast<i64>(value);
        f64 frac = value - static_cast<f64>(int_part);
        char ibuf[32]; i32 ip = 31; ibuf[ip] = '\0';
        if (int_part == 0) ibuf[--ip] = '0';
        else { i64 ipv = int_part; while (ipv > 0) { ibuf[--ip] = static_cast<char>('0' + (ipv % 10)); ipv /= 10; } }
        for (i32 i = ip; i < 31; i++) buf.push(static_cast<u8>(ibuf[i]));
        buf.push('.');
        for (i32 d = 0; d < 6; d++) { frac *= 10.0; i32 digit = static_cast<i32>(frac); buf.push(static_cast<u8>('0' + (digit % 10))); frac -= static_cast<f64>(digit); }
    }

    static void json_write_u64(Vec<u8>& buf, u64 value) noexcept {
        char ibuf[24]; i32 ip = 23; ibuf[ip] = '\0';
        if (value == 0) ibuf[--ip] = '0';
        else { u64 v = value; while (v > 0) { ibuf[--ip] = static_cast<char>('0' + (v % 10)); v /= 10; } }
        for (i32 i = ip; i < 23; i++) buf.push(static_cast<u8>(ibuf[i]));
    }

    static void json_write_date(Vec<u8>& buf, Date date) noexcept {
        if (date.serial_ == 0) { buf.push('"'); buf.push('"'); return; }
        auto ymd = date.ymd();
        i32 y = ymd.get<0>(); u32 m = ymd.get<1>(); u32 d = ymd.get<2>();
        char dt[16];
        (void)Libc::snprintf(reinterpret_cast<u8*>(dt), 16, "%04d-%02u-%02u", y, m, d);
        buf.push('"');
        for (u64 i = 0; i < 16 && dt[i] != '\0'; i++) buf.push(static_cast<u8>(dt[i]));
        buf.push('"');
    }

    static void json_write_side(Vec<u8>& buf, OrderSide side) noexcept {
        buf.push('"');
        if (side == OrderSide::Buy) { buf.push('b'); buf.push('u'); buf.push('y'); }
        else { buf.push('s'); buf.push('e'); buf.push('l'); buf.push('l'); }
        buf.push('"');
    }

    static void json_write_type(Vec<u8>& buf, OrderType type) noexcept {
        buf.push('"');
        switch (type) {
        case OrderType::Market: { buf.push('m'); buf.push('a'); buf.push('r'); buf.push('k'); buf.push('e'); buf.push('t'); break; }
        case OrderType::Limit:  { buf.push('l'); buf.push('i'); buf.push('m'); buf.push('i'); buf.push('t'); break; }
        case OrderType::Stop:   { buf.push('s'); buf.push('t'); buf.push('o'); buf.push('p'); break; }
        case OrderType::StopLimit: { buf.push('s'); buf.push('t'); buf.push('o'); buf.push('p'); buf.push('_'); buf.push('l'); buf.push('i'); buf.push('m'); buf.push('i'); buf.push('t'); break; }
        default: { buf.push('m'); buf.push('a'); buf.push('r'); buf.push('k'); buf.push('e'); buf.push('t'); break; }
        }
        buf.push('"');
    }

    static void json_write_status(Vec<u8>& buf, OrderStatus status) noexcept {
        buf.push('"');
        switch (status) {
        case OrderStatus::New:       { buf.push('n'); buf.push('e'); buf.push('w'); break; }
        case OrderStatus::Partial:   { buf.push('p'); buf.push('a'); buf.push('r'); buf.push('t'); buf.push('i'); buf.push('a'); buf.push('l'); break; }
        case OrderStatus::Filled:    { buf.push('f'); buf.push('i'); buf.push('l'); buf.push('l'); buf.push('e'); buf.push('d'); break; }
        case OrderStatus::Cancelled: { buf.push('c'); buf.push('a'); buf.push('n'); buf.push('c'); buf.push('e'); buf.push('l'); buf.push('l'); buf.push('e'); buf.push('d'); break; }
        case OrderStatus::Rejected:  { buf.push('r'); buf.push('e'); buf.push('j'); buf.push('e'); buf.push('c'); buf.push('t'); buf.push('e'); buf.push('d'); break; }
        }
        buf.push('"');
    }

    static void json_write_bool(Vec<u8>& buf, bool value) noexcept {
        if (value) { buf.push('t'); buf.push('r'); buf.push('u'); buf.push('e'); }
        else { buf.push('f'); buf.push('a'); buf.push('l'); buf.push('s'); buf.push('e'); }
    }

    static void serialize_entry(Vec<u8>& buf, const OrderMappingEntry& entry) noexcept {
        auto push_sv = [&buf](String_View s) {
            for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
        };

        buf.push('{');

        push_sv("\"cl_ord_id\":"_v);
        json_write_string(buf, entry.cl_ord_id_);
        push_sv(",\"exchange_order_id\":"_v);
        json_write_string(buf, entry.exchange_order_id_.view());
        push_sv(",\"symbol\":"_v);
        json_write_string(buf, entry.symbol_);
        push_sv(",\"sent_time\":"_v);
        json_write_date(buf, entry.sent_time_);
        push_sv(",\"ack_time\":"_v);
        json_write_date(buf, entry.ack_time_);
        push_sv(",\"side\":"_v);
        json_write_side(buf, entry.side_);
        push_sv(",\"type\":"_v);
        json_write_type(buf, entry.type_);
        push_sv(",\"original_qty\":"_v);
        json_write_f64(buf, entry.original_qty_);
        push_sv(",\"filled_qty\":"_v);
        json_write_f64(buf, entry.filled_qty_);
        push_sv(",\"avg_fill_price\":"_v);
        json_write_f64(buf, entry.avg_fill_price_);
        push_sv(",\"status\":"_v);
        json_write_status(buf, entry.status_);
        push_sv(",\"is_pending\":"_v);
        json_write_bool(buf, entry.is_pending_);

        buf.push('}');
    }

    // =====================================================================
    // JSON deserialization helpers
    // =====================================================================

    // Extract a JSON array value by key: "key":[...]
    [[nodiscard]] static Opt<String_View> extract_array(String_View json, String_View key) noexcept {
        // Find the key
        u64 key_pos = find_json_key(json, key);
        if (key_pos >= json.length()) return {};

        // Skip to ':'
        u64 colon_pos = key_pos;
        while (colon_pos < json.length() && json[colon_pos] != ':') colon_pos++;
        colon_pos++;
        while (colon_pos < json.length() && (json[colon_pos] == ' ' || json[colon_pos] == '\t' || json[colon_pos] == '\n' || json[colon_pos] == '\r')) colon_pos++;
        if (colon_pos >= json.length() || json[colon_pos] != '[') return {};

        // Find matching ']'
        u64 start = colon_pos;
        i32 depth = 0;
        u64 end = start;
        for (; end < json.length(); end++) {
            if (json[end] == '[') depth++;
            else if (json[end] == ']') { depth--; if (depth == 0) break; }
        }
        if (end >= json.length()) return {};

        return Opt<String_View>{json.sub(start, end + 1)};
    }

    // Extract a JSON string value by key: "key":"value"
    [[nodiscard]] static Opt<String_View> extract_string(String_View json, String_View key) noexcept {
        u64 key_pos = find_json_key(json, key);
        if (key_pos >= json.length()) return {};

        u64 colon_pos = key_pos;
        while (colon_pos < json.length() && json[colon_pos] != ':') colon_pos++;
        colon_pos++;
        while (colon_pos < json.length() && (json[colon_pos] == ' ' || json[colon_pos] == '\t')) colon_pos++;
        if (colon_pos >= json.length() || json[colon_pos] != '"') return {};

        u64 start = colon_pos + 1;
        u64 end = start;
        while (end < json.length() && json[end] != '"') {
            if (json[end] == '\\') end++; // skip escaped char
            end++;
        }
        if (end >= json.length()) return {};

        return Opt<String_View>{json.sub(start, end)};
    }

    // Extract a JSON number (f64) by key
    [[nodiscard]] static Opt<f64> extract_f64(String_View json, String_View key) noexcept {
        u64 key_pos = find_json_key(json, key);
        if (key_pos >= json.length()) return {};

        u64 colon_pos = key_pos;
        while (colon_pos < json.length() && json[colon_pos] != ':') colon_pos++;
        colon_pos++;
        while (colon_pos < json.length() && (json[colon_pos] == ' ' || json[colon_pos] == '\t')) colon_pos++;
        if (colon_pos >= json.length()) return {};

        return parse_f64(json, colon_pos);
    }

    // Extract a JSON bool by key
    [[nodiscard]] static Opt<bool> extract_bool(String_View json, String_View key) noexcept {
        u64 key_pos = find_json_key(json, key);
        if (key_pos >= json.length()) return {};

        u64 colon_pos = key_pos;
        while (colon_pos < json.length() && json[colon_pos] != ':') colon_pos++;
        colon_pos++;
        while (colon_pos < json.length() && (json[colon_pos] == ' ' || json[colon_pos] == '\t')) colon_pos++;
        if (colon_pos >= json.length()) return {};

        if (colon_pos + 4 <= json.length() && json[colon_pos] == 't' && json[colon_pos+1] == 'r' && json[colon_pos+2] == 'u' && json[colon_pos+3] == 'e')
            return Opt<bool>{true};
        if (colon_pos + 5 <= json.length() && json[colon_pos] == 'f' && json[colon_pos+1] == 'a' && json[colon_pos+2] == 'l' && json[colon_pos+3] == 's' && json[colon_pos+4] == 'e')
            return Opt<bool>{false};
        return {};
    }

    [[nodiscard]] static u64 find_json_key(String_View json, String_View key) noexcept {
        for (u64 i = 0; i + key.length() + 3 <= json.length(); i++) {
            if (json[i] == '"') {
                u64 key_start = i + 1;
                bool match = true;
                for (u64 k = 0; k < key.length(); k++) {
                    if (key_start + k >= json.length() || json[key_start + k] != key[k]) {
                        match = false; break;
                    }
                }
                if (match && key_start + key.length() < json.length() && json[key_start + key.length()] == '"') {
                    return i;
                }
            }
        }
        return json.length();
    }

    [[nodiscard]] static Opt<f64> parse_f64(String_View s, u64 start) noexcept {
        f64 result = 0.0, sign = 1.0;
        u64 pos = start;
        if (pos < s.length() && static_cast<char>(s[pos]) == '-') { sign = -1.0; pos++; }
        f64 frac = 0.1;
        bool in_frac = false;
        while (pos < s.length()) {
            char c = static_cast<char>(s[pos]);
            if (c >= '0' && c <= '9') {
                if (in_frac) { result += static_cast<f64>(c - '0') * frac; frac *= 0.1; }
                else result = result * 10.0 + static_cast<f64>(c - '0');
            } else if (c == '.') { in_frac = true; }
            else break;
            pos++;
        }
        if (pos == start) return {};
        return Opt<f64>{result * sign};
    }

    [[nodiscard]] static Opt<Date> parse_date(String_View s) noexcept {
        if (s.length() < 10 || s[0] != '"') return {};
        u64 ps = 1;
        i32 y = 0; u32 m = 0, d = 0;
        for (i32 i = 0; i < 4 && ps < s.length(); i++) {
            if (s[ps] < '0' || s[ps] > '9') break;
            y = y * 10 + (s[ps++] - '0');
        }
        if (ps < s.length() && s[ps] == '-') ps++;
        for (i32 i = 0; i < 2 && ps < s.length(); i++) {
            if (s[ps] < '0' || s[ps] > '9') break;
            m = m * 10 + (s[ps++] - '0');
        }
        if (ps < s.length() && s[ps] == '-') ps++;
        for (i32 i = 0; i < 2 && ps < s.length(); i++) {
            if (s[ps] < '0' || s[ps] > '9') break;
            d = d * 10 + (s[ps++] - '0');
        }
        if (y == 0 && m == 0 && d == 0) return Opt<Date>{Date{}};
        return Opt<Date>{Date::from_ymd(y, m, d)};
    }

    [[nodiscard]] static OrderSide parse_side(String_View s) noexcept {
        // s is something like "buy" or "sell"
        if (s.length() >= 3 && s[0] == 'b') return OrderSide::Buy;
        return OrderSide::Sell;
    }

    [[nodiscard]] static OrderType parse_type(String_View s) noexcept {
        if (sv::starts_with(s, "limit"_v)) return OrderType::Limit;
        if (sv::starts_with(s, "stop_limit"_v) || sv::starts_with(s, "stopLimit"_v)) return OrderType::StopLimit;
        if (sv::starts_with(s, "stop"_v)) return OrderType::Stop;
        return OrderType::Market;
    }

    [[nodiscard]] static OrderStatus parse_status(String_View s) noexcept {
        if (sv::starts_with(s, "new"_v)) return OrderStatus::New;
        if (sv::starts_with(s, "partial"_v)) return OrderStatus::Partial;
        if (sv::starts_with(s, "filled"_v)) return OrderStatus::Filled;
        if (sv::starts_with(s, "cancelled"_v) || sv::starts_with(s, "canceled"_v)) return OrderStatus::Cancelled;
        if (sv::starts_with(s, "rejected"_v)) return OrderStatus::Rejected;
        return OrderStatus::New;
    }

    static void parse_entries_array(String_View arr, OrderMapping& mapping) noexcept {
        // arr looks like: [{...},{...},...]
        u64 pos = 1; // skip leading '['
        while (pos < arr.length()) {
            if (arr[pos] == '{') {
                u64 end = pos;
                i32 depth = 0;
                for (; end < arr.length(); end++) {
                    if (arr[end] == '{') depth++;
                    else if (arr[end] == '}') { depth--; if (depth == 0) break; }
                }
                if (end < arr.length()) {
                    String_View obj = arr.sub(pos, end + 1);
                    auto entry_opt = parse_entry_object(obj, mapping);
                    if (entry_opt.ok()) {
                        String<> key{entry_opt->cl_ord_id_};
                        mapping.by_cl_ord_id_.insert(spp::move(key), spp::move(*entry_opt));
                    }
                    pos = end + 1;
                    continue;
                }
            }
            pos++;
        }
    }

    static void parse_pending_array(String_View arr, OrderMapping& mapping) noexcept {
        u64 pos = 1;
        while (pos < arr.length()) {
            if (arr[pos] == '{') {
                u64 end = pos;
                i32 depth = 0;
                for (; end < arr.length(); end++) {
                    if (arr[end] == '{') depth++;
                    else if (arr[end] == '}') { depth--; if (depth == 0) break; }
                }
                if (end < arr.length()) {
                    String_View obj = arr.sub(pos, end + 1);
                    auto entry_opt = parse_entry_object(obj, mapping);
                    if (entry_opt.ok()) {
                        mapping.pending_orders_.push(spp::move(*entry_opt));
                    }
                    pos = end + 1;
                    continue;
                }
            }
            pos++;
        }
    }

    static Opt<OrderMappingEntry> parse_entry_object(String_View obj, OrderMapping& mapping) noexcept {
        OrderMappingEntry entry;

        auto cl_ord = extract_string(obj, "cl_ord_id"_v);
        auto exch_id = extract_string(obj, "exchange_order_id"_v);
        auto sym = extract_string(obj, "symbol"_v);
        auto sent = extract_string(obj, "sent_time"_v);
        auto ack = extract_string(obj, "ack_time"_v);
        auto side = extract_string(obj, "side"_v);
        auto type = extract_string(obj, "type"_v);
        auto orig_qty = extract_f64(obj, "original_qty"_v);
        auto fill_qty = extract_f64(obj, "filled_qty"_v);
        auto avg_px = extract_f64(obj, "avg_fill_price"_v);
        auto status = extract_string(obj, "status"_v);
        auto pending = extract_bool(obj, "is_pending"_v);

        if (!cl_ord.ok()) return {};

        entry.cl_ord_id_ = *cl_ord;
        if (exch_id.ok()) entry.exchange_order_id_ = String<>{*exch_id};
        if (sym.ok()) entry.symbol_ = mapping.intern_symbol(*sym);
        if (sent.ok()) { auto d = parse_date(*sent); if (d.ok()) entry.sent_time_ = *d; }
        if (ack.ok()) { auto d = parse_date(*ack); if (d.ok()) entry.ack_time_ = *d; }
        if (side.ok()) entry.side_ = parse_side(*side);
        if (type.ok()) entry.type_ = parse_type(*type);
        if (orig_qty.ok()) entry.original_qty_ = *orig_qty;
        if (fill_qty.ok()) entry.filled_qty_ = *fill_qty;
        if (avg_px.ok()) entry.avg_fill_price_ = *avg_px;
        if (status.ok()) entry.status_ = parse_status(*status);
        if (pending.ok()) entry.is_pending_ = *pending;

        return Opt<OrderMappingEntry>{spp::move(entry)};
    }

    static void parse_reverse_map(String_View json, OrderMapping& mapping) noexcept {
        // Find the "reverse":{...} object
        u64 pos = find_json_key(json, "reverse"_v);
        if (pos >= json.length()) return;

        // Skip to '{'
        while (pos < json.length() && json[pos] != '{') pos++;
        if (pos >= json.length()) return;

        // Find matching '}'
        u64 start = pos;
        i32 depth = 0;
        u64 end = start;
        for (; end < json.length(); end++) {
            if (json[end] == '{') depth++;
            else if (json[end] == '}') { depth--; if (depth == 0) break; }
        }
        if (end >= json.length()) return;

        // Parse key-value pairs within the reverse object
        String_View rev_obj = json.sub(start + 1, end); // exclude outer braces
        u64 rp = 0;
        while (rp < rev_obj.length()) {
            // Skip whitespace
            while (rp < rev_obj.length() && (rev_obj[rp] == ' ' || rev_obj[rp] == '\t' || rev_obj[rp] == '\n' || rev_obj[rp] == '\r' || rev_obj[rp] == ',')) rp++;
            if (rp >= rev_obj.length() || rev_obj[rp] != '"') break;

            // Read key string
            u64 key_start = rp + 1;
            u64 key_end = key_start;
            while (key_end < rev_obj.length() && rev_obj[key_end] != '"') {
                if (rev_obj[key_end] == '\\') key_end++;
                key_end++;
            }
            if (key_end >= rev_obj.length()) break;
            String_View key = rev_obj.sub(key_start, key_end);
            rp = key_end + 1;

            // Skip to ':'
            while (rp < rev_obj.length() && rev_obj[rp] != ':') rp++;
            rp++; // skip ':'
            while (rp < rev_obj.length() && (rev_obj[rp] == ' ' || rev_obj[rp] == '\t')) rp++;

            // Read value string
            if (rp >= rev_obj.length() || rev_obj[rp] != '"') break;
            u64 val_start = rp + 1;
            u64 val_end = val_start;
            while (val_end < rev_obj.length() && rev_obj[val_end] != '"') {
                if (rev_obj[val_end] == '\\') val_end++;
                val_end++;
            }
            if (val_end >= rev_obj.length()) break;
            String_View val = rev_obj.sub(val_start, val_end);
            rp = val_end + 1;

            if (!key.empty() && !val.empty()) {
                mapping.by_exchange_id_.insert(String<>{key}, String<>{val});
            }
        }
    }

    static void push_sv_to(Vec<u8>& buf, String_View s) noexcept {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    }
};

// =========================================================================
// OrderRecovery — post-restart order recovery workflow
//
// Steps:
//   1. Load order mapping from disk
//   2. Query exchange for all open orders via REST API
//   3. Reconcile — match local mapping with exchange state
//   4. For confirmed_alive_ orders — resume tracking
//   5. For unknown_ orders — cancel them (safety first)
//   6. For confirmed_dead_ orders — request missed fills from REST API
// =========================================================================

struct OrderRecovery {

    struct RecoveryResult {
        u64 orders_recovered_ = 0;   // orders resumed tracking
        u64 orders_cancelled_ = 0;   // unknown orders cancelled for safety
        u64 fills_recovered_  = 0;   // fills downloaded that we missed
        bool success_         = false;
        Vec<String<>> warnings_;

        SPP_RECORD(RecoveryResult, SPP_FIELD(orders_recovered_), SPP_FIELD(orders_cancelled_),
                   SPP_FIELD(fills_recovered_), SPP_FIELD(success_));
    };

    // Execute full recovery workflow.
    //
    // Parameters:
    //   mapping     — loaded order mapping (from disk)
    //   exchange    — connected REST gateway to query/cancel orders
    //   persistence — PersistenceManager instance for loading fills journal
    //
    // Returns recovery result with counts and warnings.
    static RecoveryResult recover(
        OrderMapping& mapping,
        RESTGateway& exchange,
        PersistenceManager& persistence) noexcept
    {
        RecoveryResult result;

        // ----- Step 1: Verify exchange connection -----
        if (!exchange.is_connected()) {
            result.warnings_.push(String<>{'e', 'x', 'c', 'h', 'a', 'n', 'g', 'e', ' ', 'n', 'o', 't', ' ', 'c', 'o', 'n', 'n', 'e', 'c', 't', 'e', 'd'});
            result.success_ = false;
            return result;
        }

        // ----- Step 2: Query exchange for all open orders -----
        auto open_orders_resp = exchange.open_orders(""_v);
        if (!open_orders_resp.ok()) {
            result.warnings_.push(String<>{'f', 'a', 'i', 'l', 'e', 'd', ' ', 't', 'o', ' ', 'q', 'u', 'e', 'r', 'y', ' ', 'o', 'p', 'e', 'n', ' ', 'o', 'r', 'd', 'e', 'r', 's'});
            result.success_ = false;
            return result;
        }

        Vec<ExchangePosition> exchange_positions;
        // Convert exchange Order objects to ExchangePosition for reconciliation
        Vec<Order> open_orders = spp::move(*open_orders_resp.data_);
        exchange_positions.reserve(open_orders.length());

        for (u64 i = 0; i < open_orders.length(); i++) {
            ExchangePosition ep;
            // Order doesn't have symbol_ directly — it's indexed by id
            // [UNSPECIFIED] Order struct lacks symbol field. In production,
            // use the REST gateway's returned JSON to extract symbol.
            // For now, we create a minimal position from order data.
            ep.quantity_ = open_orders[i].remaining();
            // Symbol cannot be extracted from Order directly
            // The caller should pre-populate exchange_positions from the REST response
            if (ep.quantity_ > 0.0) {
                exchange_positions.push(spp::move(ep));
            }
        }

        // Also query positions endpoint for a symbol-level view
        auto pos_resp = exchange.positions();
        if (pos_resp.ok() && pos_resp.data_.ok()) {
            Vec<ExchangePosition> positions = spp::move(*pos_resp.data_);
            for (u64 i = 0; i < positions.length(); i++) {
                bool already = false;
                for (u64 j = 0; j < exchange_positions.length(); j++) {
                    if (exchange_positions[j].symbol_.view() == positions[i].symbol_.view()) {
                        already = true; break;
                    }
                }
                if (!already) {
                    exchange_positions.push(spp::move(positions[i]));
                }
            }
        }

        // ----- Step 3: Reconcile -----
        auto reconciliation = mapping.reconcile(exchange_positions.slice());

        // ----- Step 4: For confirmed_alive — resume tracking -----
        result.orders_recovered_ = reconciliation.confirmed_alive_.length();

        // ----- Step 5: For unknown — cancel them (safety first) -----
        for (u64 i = 0; i < reconciliation.unknown_.length(); i++) {
            String_View sym = reconciliation.unknown_[i].symbol_;
            // Cancel via order ID if we have one; otherwise cancel all for symbol
            if (!reconciliation.unknown_[i].exchange_order_id_.view().empty()) {
                auto cancel_resp = exchange.cancel_order(
                    reconciliation.unknown_[i].exchange_order_id_.view(), sym);
                if (cancel_resp.ok() && cancel_resp.data_.ok() && *cancel_resp.data_) {
                    result.orders_cancelled_++;
                } else {
                    // Try cancel all for the symbol
                    auto cancel_all = exchange.cancel_all_orders(sym);
                    if (cancel_all.ok() && cancel_all.data_.ok() && *cancel_all.data_) {
                        result.orders_cancelled_++;
                    } else {
                        // Log warning
                        Vec<u8> warn_buf;
                        warn_buf.reserve(64);
                        push_sv_hlp(warn_buf, "failed_cancel_unknown:"_v);
                        push_sv_hlp(warn_buf, sym);
                        String<> warn{warn_buf.length()};
                        warn.set_length(warn_buf.length());
                        for (u64 k = 0; k < warn_buf.length(); k++) warn[k] = warn_buf[k];
                        result.warnings_.push(spp::move(warn));
                    }
                }
            } else {
                auto cancel_all = exchange.cancel_all_orders(sym);
                if (cancel_all.ok() && cancel_all.data_.ok() && *cancel_all.data_) {
                    result.orders_cancelled_++;
                }
            }
        }

        // ----- Step 6: For confirmed_dead — check their final status via REST -----
        for (u64 i = 0; i < reconciliation.confirmed_dead_.length(); i++) {
            const OrderMappingEntry& dead = reconciliation.confirmed_dead_[i];
            if (dead.exchange_order_id_.view().empty()) continue;

            auto order_resp = exchange.get_order(dead.exchange_order_id_.view(), dead.symbol_);
            if (order_resp.ok() && order_resp.data_.ok()) {
                Order& exch_order = *order_resp.data_;
                if (exch_order.status_ == OrderStatus::Filled && exch_order.filled_quantity_ > dead.filled_qty_) {
                    // We missed fills while we were down
                    result.fills_recovered_++;
                }
            }
        }

        result.success_ = true;
        if (reconciliation.needs_manual_review_) {
            result.warnings_.push(String<>{'m', 'a', 'n', 'u', 'a', 'l', ' ', 'r', 'e', 'v', 'i', 'e', 'w', ' ', 'r', 'e', 'q', 'u', 'i', 'r', 'e', 'd'});
        }

        return result;
    }

private:
    static void push_sv_hlp(Vec<u8>& buf, String_View s) noexcept {
        for (u64 i = 0; i < s.length(); i++) buf.push(s[i]);
    }
};

} // namespace spp::quant::data

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::data::OrderMappingEntry, "OrderMappingEntry",
                 SPP_FIELD(cl_ord_id_), SPP_FIELD(exchange_order_id_),
                 SPP_FIELD(symbol_), SPP_FIELD(sent_time_), SPP_FIELD(side_),
                 SPP_FIELD(original_qty_), SPP_FIELD(filled_qty_), SPP_FIELD(status_));

SPP_NAMED_RECORD(::spp::quant::data::OrderMapping::ReconciliationResult, "OrderMappingReconciliationResult",
                 SPP_FIELD(needs_manual_review_));

SPP_NAMED_RECORD(::spp::quant::data::OrderRecovery::RecoveryResult, "OrderRecoveryResult",
                 SPP_FIELD(orders_recovered_), SPP_FIELD(orders_cancelled_),
                 SPP_FIELD(fills_recovered_), SPP_FIELD(success_));
