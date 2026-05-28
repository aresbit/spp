#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/base/currency.h"

namespace spp::quant {

// =========================================================================
// Position — a single position (long or short) in an instrument
// =========================================================================
struct Position {
    String_View   instrument_id_ = ""_v;
    f64           quantity_      = 0.0;   ///< positive = long, negative = short
    f64           entry_price_   = 0.0;
    Date          entry_date_    = Date{};
    Currency_Code currency_      = Currency_Code::USD;

    // -------------------------------------------------------------------
    // market_value — current market value given the current price
    // -------------------------------------------------------------------
    [[nodiscard]] f64 market_value(f64 current_price) const noexcept {
        return quantity_ * current_price;
    }

    // -------------------------------------------------------------------
    // unrealized_pnl — PnL since entry
    // -------------------------------------------------------------------
    [[nodiscard]] f64 unrealized_pnl(f64 current_price) const noexcept {
        return quantity_ * (current_price - entry_price_);
    }

    // -------------------------------------------------------------------
    // is_long / is_short
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_long() const noexcept { return quantity_ > 0.0; }
    [[nodiscard]] bool is_short() const noexcept { return quantity_ < 0.0; }

    SPP_RECORD(Position, SPP_FIELD(instrument_id_), SPP_FIELD(quantity_),
               SPP_FIELD(entry_price_), SPP_FIELD(entry_date_), SPP_FIELD(currency_));
};

// =========================================================================
// PositionBook — a collection of positions with aggregate metrics
// =========================================================================
struct PositionBook {
    Vec<Position> positions_;

    // -------------------------------------------------------------------
    // total_market_value — sum of position market values
    // Requires external price map: caller provides instrument_id -> price.
    // -------------------------------------------------------------------
    [[nodiscard]] f64 total_market_value() const noexcept {
        // Without external price data, return 0.0.
        // The Portfolio struct (which holds market_prices_) provides the
        // full valuation with prices.
        return 0.0;
    }

    // -------------------------------------------------------------------
    // total_pnl — sum of unrealized PnL across all positions
    // -------------------------------------------------------------------
    [[nodiscard]] f64 total_pnl() const noexcept {
        // Same as above: requires prices from Portfolio struct.
        return 0.0;
    }

    // -------------------------------------------------------------------
    // exposure — net exposure in a given currency
    // -------------------------------------------------------------------
    [[nodiscard]] f64 exposure(Currency_Code ccy) const noexcept {
        f64 exp = 0.0;
        for (u64 i = 0; i < positions_.length(); i++) {
            if (positions_[i].currency_ == ccy) {
                exp += positions_[i].quantity_ * positions_[i].entry_price_;
            }
        }
        return exp;
    }

    // -------------------------------------------------------------------
    // net_exposure — sum of signed exposures (net directional)
    // -------------------------------------------------------------------
    [[nodiscard]] f64 net_exposure() const noexcept {
        f64 net = 0.0;
        for (u64 i = 0; i < positions_.length(); i++) {
            net += positions_[i].quantity_ * positions_[i].entry_price_;
        }
        return net;
    }

    // -------------------------------------------------------------------
    // gross_exposure — sum of absolute exposures
    // -------------------------------------------------------------------
    [[nodiscard]] f64 gross_exposure() const noexcept {
        f64 gross = 0.0;
        for (u64 i = 0; i < positions_.length(); i++) {
            f64 val = positions_[i].quantity_ * positions_[i].entry_price_;
            gross += (val >= 0.0) ? val : -val;
        }
        return gross;
    }

    // -------------------------------------------------------------------
    // leverage — gross / net (absolute ratio)
    // -------------------------------------------------------------------
    [[nodiscard]] f64 leverage() const noexcept {
        f64 net = net_exposure();
        if (net == 0.0) return 0.0;
        return gross_exposure() / Math::abs(net);
    }

    // -------------------------------------------------------------------
    // exposure_by_currency — map of currency to net exposure
    // -------------------------------------------------------------------
    [[nodiscard]] Map<Currency_Code, f64> exposure_by_currency() const noexcept {
        Map<Currency_Code, f64> result;
        for (u64 i = 0; i < positions_.length(); i++) {
            Currency_Code cc = positions_[i].currency_;
            f64 val = positions_[i].quantity_ * positions_[i].entry_price_;
            if (result.contains(cc)) {
                result[cc] += val;
            } else {
                result.insert(cc, val);
            }
        }
        return result;
    }

    // -------------------------------------------------------------------
    // add / remove / find
    // -------------------------------------------------------------------
    void add(Position pos) noexcept {
        positions_.push(spp::move(pos));
    }

    void remove(String_View id) noexcept {
        u64 n = positions_.length();
        for (u64 i = 0; i < n; i++) {
            if (positions_[i].instrument_id_ == id) {
                // Shift remaining elements left, then pop the last
                for (u64 j = i; j + 1 < n; j++) {
                    positions_[j] = spp::move(positions_[j + 1]);
                }
                positions_.pop();
                return;
            }
        }
    }

    [[nodiscard]] Opt<Position&> find(String_View id) noexcept {
        for (u64 i = 0; i < positions_.length(); i++) {
            if (positions_[i].instrument_id_ == id) {
                return Opt<Position&>{positions_[i]};
            }
        }
        return {};
    }

    [[nodiscard]] u64 size() const noexcept { return positions_.length(); }

    SPP_RECORD(PositionBook, SPP_FIELD(positions_));
};

}  // namespace spp::quant
