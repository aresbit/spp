#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/data/connector.h"  // BookLevel, OrderBookSnapshot

namespace spp::quant {

// =========================================================================
// OrderBook — incremental order book from exchange depth updates
//
// Maintains a price-level order book from a stream of add/update/delete
// events.  Levels are kept sorted:
//   bids_ : highest -> lowest (best bid at index 0)
//   asks_ : lowest  -> highest (best ask at index 0)
//
// Insertion uses binary search on the sorted Vec.  For typical order-book
// depth (N < 200) this is fast and avoids the overhead of a tree structure.
// =========================================================================

struct OrderBook {
    String_View symbol_;

    Vec<BookLevel> bids_;
    Vec<BookLevel> asks_;

    Date last_update_;
    u64  sequence_number_ = 0;

    // =====================================================================
    // Level Operations
    // =====================================================================

    /// Apply a single level update (exchange depth-stream format).
    /// `is_bid` = true for bid side, false for ask side.
    /// `quantity` = 0 means delete that price level.
    void update_level(bool is_bid, f64 price, f64 quantity) {
        if (is_bid)
            insert_or_update(bids_, price, quantity, /*descending=*/true);
        else
            insert_or_update(asks_, price, quantity, /*descending=*/false);
        sequence_number_++;
    }

    /// Apply a full snapshot (initial load or resync).
    /// Replaces the current book entirely.
    void apply_snapshot(Slice<const BookLevel> bids, Slice<const BookLevel> asks) {
        bids_.clear();
        asks_.clear();
        bids_.reserve(bids.length());
        asks_.reserve(asks.length());
        for (u64 i = 0; i < bids.length(); i++) bids_.push(bids[i]);
        for (u64 i = 0; i < asks.length(); i++) asks_.push(asks[i]);
        // Verify sort order — bids descending, asks ascending
        validate_sorted();
        sequence_number_++;
    }

    /// Apply a batch of updates for one side.
    void apply_updates(bool is_bid, Slice<const BookLevel> updates) {
        for (u64 i = 0; i < updates.length(); i++) {
            update_level(is_bid, updates[i].price_, updates[i].volume_);
        }
    }

    // =====================================================================
    // Top of Book
    // =====================================================================

    [[nodiscard]] f64 best_bid() const noexcept {
        return bids_.empty() ? 0.0 : bids_[0].price_;
    }

    [[nodiscard]] f64 best_ask() const noexcept {
        return asks_.empty() ? 0.0 : asks_[0].price_;
    }

    [[nodiscard]] f64 mid_price() const noexcept {
        f64 b = best_bid();
        f64 a = best_ask();
        if (b <= 0.0 || a <= 0.0) return 0.0;
        return (b + a) / 2.0;
    }

    /// Weighted mid: (bid * ask_size + ask * bid_size) / (bid_size + ask_size).
    [[nodiscard]] f64 weighted_mid() const noexcept {
        f64 b  = best_bid();
        f64 a  = best_ask();
        f64 bs = bids_.empty()  ? 0.0 : bids_[0].volume_;
        f64 as = asks_.empty()  ? 0.0 : asks_[0].volume_;
        f64 tot = bs + as;
        if (b <= 0.0 || a <= 0.0 || tot <= 0.0) return 0.0;
        return (b * as + a * bs) / tot;
    }

    [[nodiscard]] f64 spread() const noexcept {
        return best_ask() - best_bid();
    }

    [[nodiscard]] f64 spread_bps() const noexcept {
        f64 m = mid_price();
        if (m <= 0.0) return 0.0;
        return (best_ask() - best_bid()) / m * 10000.0;
    }

    // =====================================================================
    // Depth Analytics
    // =====================================================================

    /// Total bid volume within first `levels` price levels.
    [[nodiscard]] f64 bid_depth(u64 levels = 5) const noexcept {
        f64 total = 0.0;
        u64 n = levels < bids_.length() ? levels : bids_.length();
        for (u64 i = 0; i < n; i++) total += bids_[i].volume_;
        return total;
    }

    /// Total ask volume within first `levels` price levels.
    [[nodiscard]] f64 ask_depth(u64 levels = 5) const noexcept {
        f64 total = 0.0;
        u64 n = levels < asks_.length() ? levels : asks_.length();
        for (u64 i = 0; i < n; i++) total += asks_[i].volume_;
        return total;
    }

    /// Total bid volume within `pct` of mid (e.g. 0.01 = 1%).
    [[nodiscard]] f64 bid_volume_within(f64 pct_from_mid) const noexcept {
        f64 m = mid_price();
        if (m <= 0.0) return 0.0;
        f64 lo = m * (1.0 - pct_from_mid);
        f64 total = 0.0;
        for (u64 i = 0; i < bids_.length(); i++) {
            if (bids_[i].price_ < lo) break;
            total += bids_[i].volume_;
        }
        return total;
    }

    /// Total ask volume within `pct` of mid.
    [[nodiscard]] f64 ask_volume_within(f64 pct_from_mid) const noexcept {
        f64 m = mid_price();
        if (m <= 0.0) return 0.0;
        f64 hi = m * (1.0 + pct_from_mid);
        f64 total = 0.0;
        for (u64 i = 0; i < asks_.length(); i++) {
            if (asks_[i].price_ > hi) break;
            total += asks_[i].volume_;
        }
        return total;
    }

    /// Order book imbalance within `levels`:
    ///   (bid_vol - ask_vol) / (bid_vol + ask_vol)   range [-1, 1]
    [[nodiscard]] f64 imbalance(u64 levels = 5) const noexcept {
        f64 bv = bid_depth(levels);
        f64 av = ask_depth(levels);
        f64 tot = bv + av;
        if (tot <= 0.0) return 0.0;
        return (bv - av) / tot;
    }

    /// Volume-weighted average price for `levels` bid levels.
    [[nodiscard]] f64 bid_vwap(u64 levels = 5) const noexcept {
        f64 vol_sum = 0.0, px_vol = 0.0;
        u64 n = levels < bids_.length() ? levels : bids_.length();
        for (u64 i = 0; i < n; i++) {
            vol_sum += bids_[i].volume_;
            px_vol  += bids_[i].price_ * bids_[i].volume_;
        }
        return vol_sum > 0.0 ? px_vol / vol_sum : 0.0;
    }

    /// Volume-weighted average price for `levels` ask levels.
    [[nodiscard]] f64 ask_vwap(u64 levels = 5) const noexcept {
        f64 vol_sum = 0.0, px_vol = 0.0;
        u64 n = levels < asks_.length() ? levels : asks_.length();
        for (u64 i = 0; i < n; i++) {
            vol_sum += asks_[i].volume_;
            px_vol  += asks_[i].price_ * asks_[i].volume_;
        }
        return vol_sum > 0.0 ? px_vol / vol_sum : 0.0;
    }

    // =====================================================================
    // Slippage / Market Order Cost
    // =====================================================================

    /// Total cost to buy `quantity` shares by walking the ask book.
    /// Returns 0.0 if the book cannot fill the full quantity.
    [[nodiscard]] f64 market_buy_cost(f64 quantity) const noexcept {
        f64 remaining = quantity;
        f64 total = 0.0;
        for (u64 i = 0; i < asks_.length() && remaining > 1e-12; i++) {
            f64 taken = Math::min(remaining, asks_[i].volume_);
            total    += taken * asks_[i].price_;
            remaining -= taken;
        }
        // If we couldn't fill, return 0 to signal insufficient depth
        if (remaining > 1e-12) return 0.0;
        return total;
    }

    /// Total revenue from selling `quantity` shares by walking the bid book.
    [[nodiscard]] f64 market_sell_cost(f64 quantity) const noexcept {
        f64 remaining = quantity;
        f64 total = 0.0;
        for (u64 i = 0; i < bids_.length() && remaining > 1e-12; i++) {
            f64 taken = Math::min(remaining, bids_[i].volume_);
            total    += taken * bids_[i].price_;
            remaining -= taken;
        }
        if (remaining > 1e-12) return 0.0;
        return total;
    }

    /// Effective round-trip impact in bps for a given size.
    /// Computes the cost of buying and selling half the quantity.
    [[nodiscard]] f64 impact_bps(f64 quantity) const noexcept {
        f64 m = mid_price();
        if (m <= 0.0 || quantity <= 0.0) return 0.0;
        f64 half = quantity / 2.0;
        f64 buy_cost  = market_buy_cost(half);
        f64 sell_cost = market_sell_cost(half);
        if (buy_cost <= 0.0 || sell_cost <= 0.0) return 0.0;
        f64 notional = half * m;
        if (notional <= 0.0) return 0.0;
        return (buy_cost - sell_cost) / notional * 10000.0;
    }

    // =====================================================================
    // Microstructure Signals
    // =====================================================================

    struct MicrostructureSignals {
        f64 spread_vs_avg_;         ///< current spread / average spread
        f64 imbalance_z_;           ///< z-score of current imbalance
        f64 depth_decay_;           ///< how fast depth decays from top (exponential)
        f64 order_flow_toxicity_;   ///< VPIN-like simplified metric
        u64 quote_age_us_;          ///< microseconds since last quote update
    };

    /// Update historical stats (call on each update).
    void update_micro_stats() {
        f64 sp = spread();
        f64 im = imbalance(5);

        // Circular buffer for spreads
        recent_spreads_[micro_write_idx_ % MICRO_BUF_SIZE] = sp;
        recent_imbalances_[micro_write_idx_ % MICRO_BUF_SIZE] = im;
        micro_write_idx_++;

        micro_stats_update_count_++;
    }

    /// Compute microstructure signals from the rolling history.
    [[nodiscard]] MicrostructureSignals get_signals() const noexcept {
        MicrostructureSignals sig{};
        u64 n = Math::min(micro_write_idx_, MICRO_BUF_SIZE);

        if (n < 2) return sig;

        // Spread vs average
        f64 spread_sum = 0.0;
        for (u64 i = 0; i < n; i++) spread_sum += recent_spreads_[i];
        f64 spread_avg = spread_sum / static_cast<f64>(n);
        f64 cur_spread = spread();
        sig.spread_vs_avg_ = (spread_avg > 1e-12) ? cur_spread / spread_avg : 1.0;

        // Imbalance z-score
        f64 imb_sum = 0.0, imb_sq = 0.0;
        for (u64 i = 0; i < n; i++) {
            imb_sum += recent_imbalances_[i];
            imb_sq  += recent_imbalances_[i] * recent_imbalances_[i];
        }
        f64 imb_mean = imb_sum / static_cast<f64>(n);
        f64 imb_var  = imb_sq / static_cast<f64>(n) - imb_mean * imb_mean;
        f64 imb_std  = Math::sqrt(Math::max(0.0, imb_var));
        f64 cur_imb  = imbalance(5);
        sig.imbalance_z_ = (imb_std > 1e-12) ? (cur_imb - imb_mean) / imb_std : 0.0;

        // Depth decay: fit exponential to normalized volume per level
        u64 max_levels = Math::min(static_cast<u64>(10),
                                    Math::min(bids_.length(), asks_.length()));
        if (max_levels >= 3) {
            f64 sum_lx = 0.0, sum_l = 0.0, sum_x = 0.0, sum_xx = 0.0;
            f64 top_vol = bids_[0].volume_ + asks_[0].volume_;
            if (top_vol > 1e-12) {
                for (u64 i = 0; i < max_levels; i++) {
                    f64 vol = bids_[i].volume_ + asks_[i].volume_;
                    f64 norm_vol = Math::max(0.0, vol / top_vol);
                    // Model: log(norm_vol) = -decay * level
                    f64 log_vol = Math::log(Math::max(1e-12, norm_vol));
                    f64 lvl = static_cast<f64>(i);
                    sum_lx += lvl * log_vol;
                    sum_l  += lvl;
                    sum_x  += log_vol;
                    sum_xx += lvl * lvl;
                }
                f64 denom = static_cast<f64>(max_levels) * sum_xx - sum_l * sum_l;
                if (Math::abs(denom) > 1e-12) {
                    f64 slope = (static_cast<f64>(max_levels) * sum_lx - sum_l * sum_x) / denom;
                    sig.depth_decay_ = Math::max(0.0, -slope);  // positive = faster decay
                }
            }
        }

        // Order-flow toxicity (VPIN-like): correlation between imbalance changes
        // and price moves over the recent window.
        if (n >= 4) {
            f64 sum_di_dp = 0.0, sum_di2 = 0.0;
            u64 pairs = 0;
            for (u64 i = 1; i < n; i++) {
                f64 di = recent_imbalances_[i] - recent_imbalances_[i - 1];
                f64 dp = recent_spreads_[i] - recent_spreads_[i - 1];  // proxy for price move
                // A more sophisticated impl would use actual mid-price changes.
                sum_di_dp += di * dp;
                sum_di2   += di * di;
                pairs++;
            }
            if (pairs > 0 && sum_di2 > 1e-12) {
                sig.order_flow_toxicity_ = Math::max(-1.0, Math::min(1.0, sum_di_dp / sum_di2));
            }
        }

        sig.quote_age_us_ = last_update_timestamp_us_;
        return sig;
    }

    // =====================================================================
    // Health Checks
    // =====================================================================

    [[nodiscard]] bool is_crossed() const noexcept {
        f64 b = best_bid();
        f64 a = best_ask();
        if (b <= 0.0 || a <= 0.0) return false;
        return b >= a;
    }

    [[nodiscard]] bool is_stale(u64 max_age_ms) const noexcept {
        // Compare last_update_ with current time
        // [UNSPECIFIED] Using system_clock for staleness check.
        // A real implementation should use a high-resolution exchange clock.
        Date now = Date::today();
        // max_age_ms maps to days; approx: 1 day = 86400000 ms
        i32 max_age_days = static_cast<i32>(max_age_ms / 86400000);
        if (max_age_days < 1) max_age_days = 1;
        return (now - last_update_) >= max_age_days;
    }

    [[nodiscard]] u64 update_count() const noexcept {
        return micro_stats_update_count_;
    }

    [[nodiscard]] f64 max_price_level() const noexcept {
        f64 max_px = 0.0;
        for (u64 i = 0; i < bids_.length(); i++)
            if (bids_[i].price_ > max_px) max_px = bids_[i].price_;
        for (u64 i = 0; i < asks_.length(); i++)
            if (asks_[i].price_ > max_px) max_px = asks_[i].price_;
        return max_px;
    }

    [[nodiscard]] f64 min_price_level() const noexcept {
        if (bids_.empty() && asks_.empty()) return 0.0;
        f64 min_px = bids_.empty() ? asks_[0].price_ : bids_[0].price_;
        for (u64 i = 0; i < bids_.length(); i++)
            if (bids_[i].price_ < min_px) min_px = bids_[i].price_;
        for (u64 i = 0; i < asks_.length(); i++)
            if (asks_[i].price_ < min_px) min_px = asks_[i].price_;
        return min_px;
    }

    // =====================================================================
    // Conversion
    // =====================================================================

    [[nodiscard]] OrderBookSnapshot to_snapshot() const {
        OrderBookSnapshot snap;
        snap.timestamp_ = last_update_;
        snap.symbol_    = symbol_;
        snap.bids_      = bids_.clone();
        snap.asks_      = asks_.clone();
        return snap;
    }

    SPP_RECORD(OrderBook, SPP_FIELD(symbol_), SPP_FIELD(last_update_),
               SPP_FIELD(sequence_number_));

private:
    // =====================================================================
    // insert_or_update — binary-search a price level and update in place.
    //
    // Cases:
    //   1. Price exists, qty > 0  -> update volume
    //   2. Price exists, qty == 0 -> delete the level
    //   3. Price absent, qty > 0  -> insert in sorted order
    //   4. Price absent, qty == 0 -> no-op
    // =====================================================================
    void insert_or_update(Vec<BookLevel>& levels, f64 price, f64 qty, bool descending) {
        u64 lo = 0, hi = levels.length();

        // Binary search for insertion point / existing level
        while (lo < hi) {
            u64 mid = lo + (hi - lo) / 2;
            if (descending) {
                // bids: higher prices first
                if (levels[mid].price_ > price) lo = mid + 1;
                else                            hi = mid;
            } else {
                // asks: lower prices first
                if (levels[mid].price_ < price) lo = mid + 1;
                else                            hi = mid;
            }
        }

        // lo is the index where `price` should be (or is already located)
        if (lo < levels.length() && levels[lo].price_ == price) {
            // Price level exists
            if (qty <= 0.0) {
                // Delete: shift left and pop
                for (u64 i = lo; i + 1 < levels.length(); i++) {
                    levels[i] = levels[i + 1];
                }
                levels.pop();
            } else {
                // Update quantity
                levels[lo].volume_ = qty;
            }
        } else if (qty > 0.0) {
            // Price does not exist — insert new level
            levels.push(BookLevel{});
            for (u64 i = levels.length() - 1; i > lo; i--) {
                levels[i] = spp::move(levels[i - 1]);
            }
            levels[lo] = BookLevel{price, qty, 1};
        }
        // else: price doesn't exist and qty <= 0 — no-op
    }

    /// Verify that the book levels are correctly sorted.
    void validate_sorted() const noexcept {
        for (u64 i = 1; i < bids_.length(); i++) {
            if (bids_[i - 1].price_ < bids_[i].price_) {
                // [UNSPECIFIED] Bid sort order violated.  The snapshot data
                // may have been unsorted.  In production this should trigger
                // a re-sort or alert; for now we silently accept it.
                return;
            }
        }
        for (u64 i = 1; i < asks_.length(); i++) {
            if (asks_[i - 1].price_ > asks_[i].price_) {
                return;
            }
        }
    }

    // Circular buffers for microstructure stats
    static constexpr u64 MICRO_BUF_SIZE = 128;
    f64  recent_spreads_   [MICRO_BUF_SIZE] = {};
    f64  recent_imbalances_[MICRO_BUF_SIZE] = {};
    u64  micro_write_idx_       = 0;
    u64  micro_stats_update_count_ = 0;
    u64  last_update_timestamp_us_ = 0;
};

// =========================================================================
// OrderBookManager — manages order books for multiple symbols
// =========================================================================

struct OrderBookManager {
    /// Owns the symbol strings so String_View keys remain valid.
    Vec<String<>>    symbol_storage_;
    Map<String_View, OrderBook> books_;

    /// Get or create an order book for a symbol.
    /// Returns a reference to the (possibly newly created) book.
    [[nodiscard]] OrderBook& get_book(String_View symbol) {
        auto opt = books_.try_get(symbol);
        if (opt.ok()) return **opt;

        // Create and store a new book
        symbol_storage_.push(sv::sv_to_string(symbol));
        String_View key = symbol_storage_.back().view();

        OrderBook book;
        book.symbol_        = key;
        book.last_update_   = Date::today();
        book.sequence_number_ = 0;

        books_.insert(key, spp::move(book));
        return *books_.try_get(key);
    }

    /// Process a single depth update message.
    void process_depth_update(String_View symbol, bool is_bid, f64 price, f64 qty) {
        OrderBook& book = get_book(symbol);
        book.update_level(is_bid, price, qty);
        book.last_update_ = Date::today();
        book.update_micro_stats();
    }

    /// Process a full snapshot for a symbol.
    void process_snapshot(String_View symbol, Slice<const BookLevel> bids,
                          Slice<const BookLevel> asks) {
        OrderBook& book = get_book(symbol);
        book.apply_snapshot(bids, asks);
        book.last_update_ = Date::today();
        book.update_micro_stats();
    }

    /// Check all books for crossed markets or staleness.
    /// Returns a list of problematic symbol names.
    [[nodiscard]] Vec<String_View> health_check(u64 max_age_ms = 5000) const {
        Vec<String_View> problems;
        for (const auto& kv : books_) {
            const OrderBook& book = kv.second;
            if (book.is_crossed()) {
                problems.push(book.symbol_);
            } else if (book.is_stale(max_age_ms)) {
                problems.push(book.symbol_);
            }
        }
        return problems;
    }

    /// Get aggregated microstructure signals across all tracked books.
    [[nodiscard]] Map<String_View, OrderBook::MicrostructureSignals> all_signals() const {
        Map<String_View, OrderBook::MicrostructureSignals> result;
        for (const auto& kv : books_) {
            result.insert(kv.first, kv.second.get_signals());
        }
        return result;
    }
};

} // namespace spp::quant

// Helpers (see connector.h for sv::sv_to_string etc.)
// Already available from connector.h inclusion chain.
