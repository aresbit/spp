#pragma once

#include <spp/core/base.h>
#include <spp/reflection/json.h>

#include <binance/models.h>

namespace spp::App::Binance {

// L2 order book maintained from a REST snapshot + a stream of WebSocket diff
// updates per Binance Spot's documented protocol. Sequence-number gaps abort
// the merge so callers can resubscribe + resnapshot instead of silently
// drifting.
//
// Binance encodes each price level as a JSON tuple `["price", "qty"]` rather
// than an object — so the wire form uses Vec<String> per level and we convert
// to the friendlier {price, qty} struct on ingest.

using Wire_Level = Vec<String<Mdefault>, Mdefault>; // [price, qty]

// REST GET /api/v3/depth?symbol=...&limit=...
struct Depth_Snapshot {
    i64 lastUpdateId = 0;
    Vec<Wire_Level, Mdefault> bids;
    Vec<Wire_Level, Mdefault> asks;
};

// WS @depth diff event (the JSON payload after stripping the stream wrapper).
struct Depth_Update {
    String<Mdefault> e; // "depthUpdate"
    i64 E = 0;          // event time (ms)
    String<Mdefault> s; // symbol
    i64 U = 0;          // first updateId in this event
    i64 u = 0;          // final updateId in this event
    Vec<Wire_Level, Mdefault> b; // bid updates
    Vec<Wire_Level, Mdefault> a; // ask updates
};

[[nodiscard]] inline Order_Book_Level wire_to_level(const Wire_Level& w) noexcept {
    Order_Book_Level out;
    if(w.length() >= 1) out.price = w[0].view().string<Mdefault>();
    if(w.length() >= 2) out.qty = w[1].view().string<Mdefault>();
    return out;
}

// Local order book. Bids descending (best = highest price first), asks
// ascending. Both kept as sorted Vecs because typical depth used in
// strategies (≤ a few hundred levels) is small enough that array operations
// outperform a balanced tree on cache locality.
struct Order_Book {
    Vec<Order_Book_Level, Mdefault> bids;
    Vec<Order_Book_Level, Mdefault> asks;
    i64 last_update_id = 0;

    enum class Merge_Status : u8 {
        ok,
        stale_update,   // u <= last_update_id; safe to skip
        gap,            // sequence break — caller must resubscribe
        invalid_format, // qty/price parse failed
    };

    [[nodiscard]] static Order_Book from_snapshot(const Depth_Snapshot& s) noexcept {
        Order_Book b;
        b.last_update_id = s.lastUpdateId;
        for(const auto& w : s.bids) b.bids.push(wire_to_level(w));
        for(const auto& w : s.asks) b.asks.push(wire_to_level(w));
        sort_bids_(b.bids);
        sort_asks_(b.asks);
        return b;
    }

    // Apply one diff event. Caller must hand updates in increasing-`U` order.
    [[nodiscard]] Merge_Status apply(const Depth_Update& update) noexcept {
        // Skip events fully covered by the snapshot.
        if(update.u <= last_update_id) return Merge_Status::stale_update;
        // First update after snapshot must straddle lastUpdateId+1.
        if(last_update_id == 0) {
            // No snapshot installed — accept the first update unconditionally.
        } else if(update.U > last_update_id + 1) {
            return Merge_Status::gap;
        }
        for(const auto& w : update.b) {
            auto lvl = wire_to_level(w);
            if(!apply_level_(bids, lvl, /*is_bid=*/true)) return Merge_Status::invalid_format;
        }
        for(const auto& w : update.a) {
            auto lvl = wire_to_level(w);
            if(!apply_level_(asks, lvl, /*is_bid=*/false)) return Merge_Status::invalid_format;
        }
        last_update_id = update.u;
        return Merge_Status::ok;
    }

    [[nodiscard]] Opt<Order_Book_Level> best_bid() const noexcept {
        if(bids.empty()) return {};
        return Opt<Order_Book_Level>{clone_level_(bids.front())};
    }
    [[nodiscard]] Opt<Order_Book_Level> best_ask() const noexcept {
        if(asks.empty()) return {};
        return Opt<Order_Book_Level>{clone_level_(asks.front())};
    }

    // True if the book has at least one bid and one ask AND the spread is
    // non-negative when prices are parsed as Decimal_8. We don't expose the
    // Decimal conversion here — callers do their own parsing if needed.
    [[nodiscard]] bool ok() const noexcept {
        return !bids.empty() && !asks.empty();
    }

private:
    [[nodiscard]] static bool price_less_(String_View a, String_View b) noexcept {
        // Lexicographic comparison after equal-length zero-padding is correct
        // for non-negative decimal-as-string values. We canonicalise by
        // comparing integer-part lengths first, then char-wise.
        u64 a_dot = a.length(), b_dot = b.length();
        for(u64 i = 0; i < a.length(); i++) if(a[i] == '.') { a_dot = i; break; }
        for(u64 i = 0; i < b.length(); i++) if(b[i] == '.') { b_dot = i; break; }
        if(a_dot != b_dot) return a_dot < b_dot;
        for(u64 i = 0; i < a.length() && i < b.length(); i++) {
            if(a[i] != b[i]) return a[i] < b[i];
        }
        return a.length() < b.length();
    }

    [[nodiscard]] static bool price_equal_(String_View a, String_View b) noexcept {
        if(a.length() != b.length()) return false;
        for(u64 i = 0; i < a.length(); i++) if(a[i] != b[i]) return false;
        return true;
    }

    [[nodiscard]] static bool is_zero_qty_(String_View q) noexcept {
        // qty=0 in Binance diff messages means "remove this price level".
        // Format is something like "0.00000000".
        bool seen_nonzero = false;
        for(u8 c : q) {
            if(c == '.' || c == '0') continue;
            seen_nonzero = true;
            break;
        }
        return !seen_nonzero;
    }

    [[nodiscard]] static Order_Book_Level
    clone_level_(const Order_Book_Level& src) noexcept {
        Order_Book_Level out;
        out.price = src.price.view().string<Mdefault>();
        out.qty = src.qty.view().string<Mdefault>();
        return out;
    }

    static void sort_bids_(Vec<Order_Book_Level, Mdefault>& v) noexcept {
        // Descending by price.
        for(u64 i = 1; i < v.length(); i++) {
            for(u64 j = i; j > 0 && price_less_(v[j - 1].price.view(), v[j].price.view());
                j--) {
                swap(v[j - 1], v[j]);
            }
        }
    }
    static void sort_asks_(Vec<Order_Book_Level, Mdefault>& v) noexcept {
        // Ascending by price.
        for(u64 i = 1; i < v.length(); i++) {
            for(u64 j = i; j > 0 && price_less_(v[j].price.view(), v[j - 1].price.view());
                j--) {
                swap(v[j - 1], v[j]);
            }
        }
    }

    [[nodiscard]] static bool apply_level_(Vec<Order_Book_Level, Mdefault>& side,
                                            const Order_Book_Level& lvl,
                                            bool is_bid) noexcept {
        if(lvl.price.length() == 0) return false;
        // Find existing matching price.
        u64 idx = side.length();
        for(u64 i = 0; i < side.length(); i++) {
            if(price_equal_(side[i].price.view(), lvl.price.view())) {
                idx = i;
                break;
            }
        }
        bool remove = is_zero_qty_(lvl.qty.view());
        if(idx < side.length()) {
            if(remove) {
                // Erase by shifting down.
                for(u64 i = idx; i + 1 < side.length(); i++) {
                    swap(side[i], side[i + 1]);
                }
                side.pop();
                return true;
            }
            side[idx].qty = lvl.qty.view().string<Mdefault>();
            return true;
        }
        if(remove) {
            return true; // removing a price level that wasn't present — no-op
        }
        // Insert preserving order.
        Order_Book_Level copy = clone_level_(lvl);
        side.push(spp::move(copy));
        if(is_bid) sort_bids_(side);
        else sort_asks_(side);
        return true;
    }
};

} // namespace spp::App::Binance

namespace spp {

SPP_NAMED_RECORD(App::Binance::Depth_Snapshot, "Depth_Snapshot", SPP_FIELD(lastUpdateId),
                 SPP_FIELD(bids), SPP_FIELD(asks));
SPP_NAMED_RECORD(App::Binance::Depth_Update, "Depth_Update", SPP_FIELD(e), SPP_FIELD(E),
                 SPP_FIELD(s), SPP_FIELD(U), SPP_FIELD(u), SPP_FIELD(b), SPP_FIELD(a));

} // namespace spp
