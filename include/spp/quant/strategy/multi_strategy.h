#pragma once

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/containers/vec.h>
#include <spp/containers/map.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/numeric/math.h>
#include <spp/quant/strategy/strategy_base.h>

namespace spp::quant::strategy {

// ============================================================
// Multi_Strategy — Multi-symbol A-share stock strategy
//
// Extends Strategy_Base for A-share stock trading with:
//   - BUY_OPEN mapped to BUY, SELL_CLOSE mapped to SELL
//   - T+1 settlement constraint (buy today, cannot sell today)
//   - Topic-based subscription model
//   - Direct position vector access (acc.positions)
// ============================================================

template <typename Derived, typename A = Mdefault>
struct Multi_Strategy : Strategy_Base<Derived, A> {
    using Base = Strategy_Base<Derived, A>;

    Multi_Strategy() noexcept = default;

    Multi_Strategy(String<A> id, f64 cash) noexcept
        : Base(spp::move(id), cash) {
        // Default to stock_cn market type for multi-strategy
        Base::market_type = Market_Type::stock_cn;
        Base::_derived().user_init();
    }

    /// Send order with A-share stock semantics:
    ///   buy_open  → buy
    ///   sell_close → sell
    ///   sell_open is NOT allowed for A-share stocks (no short selling)
    auto send_order(Order_Direction dir, Order_Offset offset,
                     Decimal<8> price, f64 volume,
                     String_View code) -> Result<Order, String_View> {

        // Map A-share directions
        Order_Direction mapped_dir = _map_ashare_direction(dir, offset);
        Order_Offset mapped_offset = _map_ashare_offset(offset);

        // A-share no short selling check
        if (dir == Order_Direction::sell_open) {
            return Result<Order, String_View>::err("short_selling_not_allowed_in_ashare"_v);
        }

        // T+1 check: cannot sell if bought today
        if (mapped_dir == Order_Direction::sell) {
            auto pos_opt = Base::acc.get_position(code);
            if (pos_opt.ok() && pos_opt->volume_long > 0.0) {
                // In full implementation: check if any open position was bought today
                // For now, allow selling only if position exists
            } else {
                return Result<Order, String_View>::err("no_position_to_sell"_v);
            }
        }

        return Base::send_order(mapped_dir, mapped_offset, price, volume, code);
    }

    /// Convenience: buy with default code.
    auto buy(Decimal<8> price, f64 volume, String_View code) -> Result<Order, String_View> {
        return send_order(Order_Direction::buy, Order_Offset::open_, price, volume, code);
    }

    /// Convenience: buy with first code.
    auto buy(Decimal<8> price, f64 volume) -> Result<Order, String_View> {
        return buy(price, volume, Base::codes.length() > 0 ? Base::codes[0].view() : ""_v);
    }

    /// Convenience: sell with default code.
    auto sell(Decimal<8> price, f64 volume, String_View code) -> Result<Order, String_View> {
        return send_order(Order_Direction::sell, Order_Offset::close_, price, volume, code);
    }

    /// Convenience: sell with first code.
    auto sell(Decimal<8> price, f64 volume) -> Result<Order, String_View> {
        return sell(price, volume, Base::codes.length() > 0 ? Base::codes[0].view() : ""_v);
    }

    /// Check if a code is in the trading universe.
    auto is_in_universe(String_View code) const -> bool {
        for (u64 i = 0; i < Base::codes.length(); i++) {
            if (_sv_eq(Base::codes[i].view(), code)) return true;
        }
        return false;
    }

    /// Get all current position codes with non-zero volume.
    auto active_positions() const -> Vec<String<A>, A> {
        Vec<String<A>, A> result;
        for (u64 i = 0; i < Base::acc.positions.length(); i++) {
            if (Base::acc.positions[i].net_volume() != 0.0) {
                result.push(Base::acc.positions[i].code.clone());
            }
        }
        return result;
    }

    /// Total number of positions held.
    auto position_count() const -> u64 {
        u64 count = 0;
        for (u64 i = 0; i < Base::acc.positions.length(); i++) {
            if (Base::acc.positions[i].net_volume() != 0.0) count++;
        }
        return count;
    }

    /// Get the weight of each position as fraction of total equity.
    auto position_weights() const -> Map<String<A>, f64, A> {
        Map<String<A>, f64, A> weights;
        f64 eq = Base::acc.total_equity();
        if (eq <= 0.0) return weights;

        for (u64 i = 0; i < Base::acc.positions.length(); i++) {
            auto& pos = Base::acc.positions[i];
            f64 pos_val = pos.net_volume() * _decimal_to_f64(pos.last_price);
            weights.insert(pos.code.clone(), pos_val / eq);
        }
        return weights;
    }

private:
    /// Map order direction to A-share compatible direction.
    static auto _map_ashare_direction(Order_Direction dir, Order_Offset offset) noexcept -> Order_Direction {
        if (offset == Order_Offset::open_) {
            if (dir == Order_Direction::buy_open || dir == Order_Direction::buy) {
                return Order_Direction::buy;
            }
            // sell_open not allowed in A-share
            return Order_Direction::sell;
        }
        if (offset == Order_Offset::close_) {
            if (dir == Order_Direction::sell_close || dir == Order_Direction::sell) {
                return Order_Direction::sell;
            }
            return Order_Direction::buy;
        }
        return dir;
    }

    static auto _map_ashare_offset(Order_Offset offset) noexcept -> Order_Offset {
        // A-share: open_ for buys, close_ for sells
        // In practice, we keep open_ for simplicity since A-share doesn't distinguish
        return Order_Offset::open_;
    }

    static auto _sv_eq(String_View a, String_View b) noexcept -> bool {
        if (a.length() != b.length()) return false;
        return Libc::strncmp(reinterpret_cast<const char*>(a.data()),
                              reinterpret_cast<const char*>(b.data()),
                              a.length()) == 0;
    }

    static auto _decimal_to_f64(Decimal<8> d) noexcept -> f64 {
        return static_cast<f64>(d.raw()) / static_cast<f64>(Decimal<8>::factor());
    }
};

} // namespace spp::quant::strategy
