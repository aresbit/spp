#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/backtest/event.h"
#include "spp/quant/backtest/cost_model.h"

namespace spp::quant::backtest {

// =========================================================================
// TransactionCost — simplified cost model (used by SimulatedBroker)
// =========================================================================
struct TransactionCost {
    f64 commission_per_share_ = 0.0;
    f64 commission_per_trade_ = 0.0;
    f64 commission_pct_       = 0.001;  // 10 bps
    f64 min_commission_       = 0.0;
    f64 spread_pct_           = 0.0001; // half-spread for market orders
    f64 market_impact_        = 0.0;    // additional flat impact fraction

    // -------------------------------------------------------------------
    // calculate — total transaction cost in absolute dollars
    // -------------------------------------------------------------------
    [[nodiscard]] f64 calculate(f64 price, f64 quantity) const noexcept {
        f64 abs_qty  = Math::abs(quantity);
        f64 notional = abs_qty * price;

        f64 comm = commission_per_share_ * abs_qty
                 + commission_per_trade_
                 + commission_pct_ * notional;
        if (comm < min_commission_) comm = min_commission_;

        f64 spread = spread_pct_ * notional;
        f64 impact = market_impact_ * notional;

        return comm + spread + impact;
    }
};

// =========================================================================
// SlippageModel — models execution price deviation from reference price
// =========================================================================
struct SlippageModel {
    enum struct Type : u8 {
        Fixed,           // constant absolute slippage
        Proportional,    // slippage = fraction of price
        SquareRoot,      // slippage ~ sqrt(qty / volume) * vol
        AlmgrenChriss    // full AC model (delegates to AlmgrenChrissImpact)
    };

    Type type_               = Type::Fixed;
    f64  fixed_slippage_     = 0.01;      // 1 cent
    f64  proportional_pct_   = 0.0005;    // 5 bps
    f64  volatility_         = 0.2;       // daily vol for sqrt/AC models
    f64  daily_volume_       = 1e6;       // ADV for sqrt/AC models
    f64  participation_rate_ = 0.1;       // for AC model
    f64  eta_                = 0.1;       // AC temporary impact coefficient
    f64  gamma_              = 0.01;      // AC permanent impact coefficient

    // -------------------------------------------------------------------
    // calculate — slippage per share (absolute price units)
    //   price:       reference/arrival price
    //   quantity:    order size in shares
    //   daily_vol:   annualized daily volatility (for sqrt/AC)
    //   daily_vol_amt: average daily volume in shares
    // -------------------------------------------------------------------
    [[nodiscard]] f64 calculate(f64 price, f64 quantity,
                                 f64 daily_vol     = 0.2,
                                 f64 daily_vol_amt = 1e6) const noexcept {
        switch (type_) {
        case Type::Fixed:
            return fixed_slippage_;

        case Type::Proportional:
            return proportional_pct_ * price;

        case Type::SquareRoot: {
            // Slippage (bp) ~ c * sigma * sqrt(Q / ADV)
            // Scale to price units
            f64 participation = Math::abs(quantity) / Math::max(daily_vol_amt, 1.0);
            f64 slippage_pct  = volatility_ * Math::sqrt(participation);
            return slippage_pct * price;
        }

        case Type::AlmgrenChriss: {
            AlmgrenChrissImpact ac;
            ac.eta_          = eta_;
            ac.gamma_        = gamma_;
            ac.sigma_        = daily_vol;
            ac.daily_volume_ = daily_vol_amt;

            // Execution over the full day
            f64 exec_frac = participation_rate_;
            if (exec_frac <= 0.0) exec_frac = 1.0;

            auto impact = ac.calculate(Math::abs(quantity), exec_frac);
            return impact.total_cost_pct * price;
        }
        }

        return 0.0;
    }
};

// =========================================================================
// SimulatedBroker — receives orders, generates fills with cost + slippage
//
// Lifecycle:
//   1. submit_order()  -> generates FillEvent(s) for market orders,
//                         enqueues limit/stop orders as pending
//   2. On each date, check pending orders against market data to see
//      if they should trigger (limit orders fill when price crosses limit;
//      stop orders trigger when price crosses stop level).
//   3. cancel_order()  -> cancels a pending order
// =========================================================================
struct SimulatedBroker {
    u64             next_order_id_ = 1;
    u64             next_fill_id_  = 1;
    TransactionCost cost_model_;
    SlippageModel   slippage_model_;
    f64             cash_          = 1'000'000.0;  // starting cash

    Map<u64, OrderEvent>     pending_orders_;    // order_id -> current order state
    Map<u64, Vec<FillEvent>> fills_by_order_;    // order_id -> all fills

    Vec<FillEvent>  all_fills_;
    Vec<OrderEvent> order_history_;

    // -------------------------------------------------------------------
    // submit_order — submit an order and receive fills
    //
    // Market orders: fill immediately at the market bar's close price.
    // Limit orders:  stored as pending; filled when market crosses limit.
    // Stop orders:   stored as pending; triggered when market crosses stop.
    // IOC:           fill what we can immediately, cancel the rest.
    // FOK:           fill all or nothing.
    // Iceberg:       treat as limit with visible quantity.
    // -------------------------------------------------------------------
    Vec<FillEvent> submit_order(const OrderEvent& order, const MarketEvent& market) {
        Vec<FillEvent> fills;

        u64 oid = order.order_id_;

        // --- Market order: fill immediately ---
        if (order.type_ == OrderType::Market) {
            f64 fill_price = market.close_;

            // Apply slippage: buy at ask (close + slip), sell at bid (close - slip)
            f64 slip_per_share = slippage_model_.calculate(market.close_, order.quantity_);
            if (order.side_ == OrderSide::Buy) {
                fill_price += slip_per_share;
            } else {
                fill_price -= slip_per_share;
            }

            f64 comm = cost_model_.calculate(fill_price, order.quantity_);

            FillEvent fill;
            fill.fill_id_    = next_fill_id_++;
            fill.order_id_   = oid;
            fill.date_       = market.date_;
            fill.side_       = order.side_;
            fill.quantity_   = order.quantity_;
            fill.price_      = fill_price;
            fill.commission_ = comm;
            fill.slippage_   = slip_per_share;
            fills.push(spp::move(fill));

            // Update cash
            f64 signed_notional = (order.side_ == OrderSide::Buy ? -1.0 : 1.0)
                                * order.quantity_ * fill_price;
            cash_ += signed_notional - comm;

            // Record
            OrderEvent filled_order = order;
            filled_order.status_ = OrderStatus::Filled;
            order_history_.push(filled_order);

            Vec<FillEvent> order_fills;
            order_fills.push(FillEvent{fills[0]});
            fills_by_order_.insert(oid, spp::move(order_fills));

        }
        // --- IOC: fill what's available at current market ---
        else if (order.type_ == OrderType::IOC) {
            // For IOC, we can always fill the full quantity at the current market
            // (simplified: assume sufficient liquidity at the limit price or market)
            bool limit_ok = (order.price_ == 0.0) ||
                (order.side_ == OrderSide::Buy  && market.close_ <= order.price_) ||
                (order.side_ == OrderSide::Sell && market.close_ >= order.price_);

            f64 fill_qty = limit_ok ? order.quantity_ : 0.0;

            if (fill_qty > 0.0) {
                f64 fill_price = (order.price_ > 0.0) ? order.price_ : market.close_;
                f64 slip_per_share = slippage_model_.calculate(fill_price, fill_qty);
                f64 comm = cost_model_.calculate(fill_price, fill_qty);

                FillEvent fill;
                fill.fill_id_    = next_fill_id_++;
                fill.order_id_   = oid;
                fill.date_       = market.date_;
                fill.side_       = order.side_;
                fill.quantity_   = fill_qty;
                fill.price_      = fill_price;
                fill.commission_ = comm;
                fill.slippage_   = slip_per_share;
                fills.push(spp::move(fill));

                f64 signed_notional = (order.side_ == OrderSide::Buy ? -1.0 : 1.0)
                                    * fill_qty * fill_price;
                cash_ += signed_notional - comm;

                OrderEvent ioc_order = order;
                ioc_order.status_ = OrderStatus::Filled;
                order_history_.push(ioc_order);

                Vec<FillEvent> order_fills;
                order_fills.push(FillEvent{fills[0]});
                fills_by_order_.insert(oid, spp::move(order_fills));
            } else {
                OrderEvent rejected = order;
                rejected.status_ = OrderStatus::Rejected;
                order_history_.push(rejected);
            }
        }
        // --- FOK: fill all or nothing ---
        else if (order.type_ == OrderType::FOK) {
            // For simplicity, FOK fills fully at the limit price if market
            // price is at or better than limit
            bool can_fill = (order.price_ == 0.0) ||
                (order.side_ == OrderSide::Buy  && market.close_ <= order.price_) ||
                (order.side_ == OrderSide::Sell && market.close_ >= order.price_);

            if (can_fill) {
                f64 fill_price = (order.price_ > 0.0) ? order.price_ : market.close_;
                f64 slip_per_share = slippage_model_.calculate(fill_price, order.quantity_);
                f64 comm = cost_model_.calculate(fill_price, order.quantity_);

                FillEvent fill;
                fill.fill_id_    = next_fill_id_++;
                fill.order_id_   = oid;
                fill.date_       = market.date_;
                fill.side_       = order.side_;
                fill.quantity_   = order.quantity_;
                fill.price_      = fill_price;
                fill.commission_ = comm;
                fill.slippage_   = slip_per_share;
                fills.push(spp::move(fill));

                f64 signed_notional = (order.side_ == OrderSide::Buy ? -1.0 : 1.0)
                                    * order.quantity_ * fill_price;
                cash_ += signed_notional - comm;

                OrderEvent fok_order = order;
                fok_order.status_ = OrderStatus::Filled;
                order_history_.push(fok_order);

                Vec<FillEvent> order_fills;
                order_fills.push(FillEvent{fills[0]});
                fills_by_order_.insert(oid, spp::move(order_fills));
            } else {
                OrderEvent rejected = order;
                rejected.status_ = OrderStatus::Rejected;
                order_history_.push(rejected);
            }
        }
        // --- Limit, Stop, StopLimit, Iceberg: store as pending ---
        else {
            pending_orders_.insert(oid, order);
            order_history_.push(order);
            // No fills yet — will be checked on subsequent market events
        }

        // Append to all_fills_
        for (u64 i = 0; i < fills.length(); i++) {
            all_fills_.push(FillEvent{fills[i]});
        }

        return fills;
    }

    // -------------------------------------------------------------------
    // check_pending — check all pending orders against new market data
    // Returns any fills generated by triggered orders.
    // -------------------------------------------------------------------
    Vec<FillEvent> check_pending(const MarketEvent& market) {
        Vec<FillEvent> new_fills;
        Vec<u64> to_remove;  // order IDs that are now done

        for (auto& kv : pending_orders_) {
            u64 oid = kv.first;
            OrderEvent& order = kv.second;

            bool should_fill = false;

            switch (order.type_) {
            case OrderType::Limit: {
                // Buy limit:  fill when price <= limit_price
                // Sell limit: fill when price >= limit_price
                // Use low/high to detect intraday crossing
                if (order.side_ == OrderSide::Buy) {
                    should_fill = market.low_ <= order.price_;
                } else {
                    should_fill = market.high_ >= order.price_;
                }
                break;
            }
            case OrderType::Stop: {
                // Buy stop:  trigger when price >= stop_price
                // Sell stop: trigger when price <= stop_price
                if (order.side_ == OrderSide::Buy) {
                    should_fill = market.high_ >= order.stop_price_;
                } else {
                    should_fill = market.low_ <= order.stop_price_;
                }
                break;
            }
            case OrderType::StopLimit: {
                // Trigger: stop level crossed -> becomes limit order
                bool triggered = false;
                if (order.side_ == OrderSide::Buy) {
                    triggered = market.high_ >= order.stop_price_;
                } else {
                    triggered = market.low_ <= order.stop_price_;
                }

                if (triggered) {
                    // Now check limit condition
                    if (order.side_ == OrderSide::Buy) {
                        should_fill = market.low_ <= order.price_;
                    } else {
                        should_fill = market.high_ >= order.price_;
                    }
                }
                break;
            }
            case OrderType::Iceberg: {
                // Iceberg: fill visible quantity at limit price when market crosses
                f64 active_qty = (order.visible_quantity_ > 0.0)
                                 ? order.visible_quantity_
                                 : order.quantity_;

                if (order.side_ == OrderSide::Buy) {
                    should_fill = market.low_ <= order.price_;
                } else {
                    should_fill = market.high_ >= order.price_;
                }

                if (should_fill) {
                    // Only fill the visible quantity per crossing
                    // Modify the order temporarily to fill partial
                    order.quantity_ = active_qty;
                }
                break;
            }
            default:
                break;
            }

            if (should_fill) {
                // Fill at limit/stop price for limit orders,
                // at market close for stop orders (triggered market)
                f64 fill_price = order.price_;
                if (order.type_ == OrderType::Stop && fill_price <= 0.0) {
                    fill_price = market.close_;
                }
                if (fill_price <= 0.0) fill_price = market.close_;

                f64 slip_per_share = slippage_model_.calculate(fill_price, order.quantity_);
                f64 comm = cost_model_.calculate(fill_price, order.quantity_);

                FillEvent fill;
                fill.fill_id_    = next_fill_id_++;
                fill.order_id_   = oid;
                fill.date_       = market.date_;
                fill.side_       = order.side_;
                fill.quantity_   = order.quantity_;
                fill.price_      = fill_price;
                fill.commission_ = comm;
                fill.slippage_   = slip_per_share;

                f64 signed_notional = (order.side_ == OrderSide::Buy ? -1.0 : 1.0)
                                    * order.quantity_ * fill_price;
                cash_ += signed_notional - comm;

                new_fills.push(spp::move(fill));
                to_remove.push(oid);

                // Update order status
                order.status_ = OrderStatus::Filled;
            }
        }

        // Remove triggered orders from pending
        for (u64 i = 0; i < to_remove.length(); i++) {
            u64 oid = to_remove[i];

            // Move fills to fills_by_order_
            // (We need to find the fill that corresponds to this order in new_fills)
            for (u64 j = 0; j < new_fills.length(); j++) {
                if (new_fills[j].order_id_ == oid) {
                    Vec<FillEvent> order_fills;
                    order_fills.push(FillEvent{new_fills[j]});
                    fills_by_order_.insert(oid, spp::move(order_fills));
                    break;
                }
            }

            pending_orders_.erase(oid);
        }

        // Append to all_fills_
        for (u64 i = 0; i < new_fills.length(); i++) {
            all_fills_.push(FillEvent{new_fills[i]});
        }

        return new_fills;
    }

    // -------------------------------------------------------------------
    // cancel_order — cancel a pending order
    // -------------------------------------------------------------------
    bool cancel_order(u64 order_id) {
        auto opt = pending_orders_.try_get(order_id);
        if (!opt.ok()) return false;

        OrderEvent& order = **opt;
        order.status_ = OrderStatus::Cancelled;
        pending_orders_.erase(order_id);
        return true;
    }

    // -------------------------------------------------------------------
    // is_filled — check if an order has been fully filled
    // -------------------------------------------------------------------
    [[nodiscard]] bool is_filled(u64 order_id) const {
        auto opt = fills_by_order_.try_get(order_id);
        return opt.ok();
    }

    // -------------------------------------------------------------------
    // remaining_quantity — quantity not yet filled for a pending order
    // -------------------------------------------------------------------
    [[nodiscard]] f64 remaining_quantity(u64 order_id) const {
        auto opt = pending_orders_.try_get(order_id);
        if (!opt.ok()) return 0.0;  // not pending anymore

        const OrderEvent& order = **opt;
        f64 filled = 0.0;

        auto fills_opt = fills_by_order_.try_get(order_id);
        if (fills_opt.ok()) {
            for (u64 i = 0; i < (*fills_opt)->length(); i++) {
                filled += (**fills_opt)[i].quantity_;
            }
        }

        return order.quantity_ - filled;
    }

    // -------------------------------------------------------------------
    // cash — current available cash
    // -------------------------------------------------------------------
    [[nodiscard]] f64 cash() const noexcept { return cash_; }

    // -------------------------------------------------------------------
    // update_cash — adjust cash balance (e.g. for dividends, fees)
    // -------------------------------------------------------------------
    void update_cash(f64 delta) noexcept { cash_ += delta; }

    // -------------------------------------------------------------------
    // reset — clear all state for a new backtest run
    // -------------------------------------------------------------------
    void reset(f64 starting_cash = 1'000'000.0) noexcept {
        next_order_id_ = 1;
        next_fill_id_  = 1;
        cash_          = starting_cash;
        pending_orders_.clear();
        fills_by_order_.clear();
        all_fills_.clear();
        order_history_.clear();
    }

    SPP_RECORD(SimulatedBroker, SPP_FIELD(next_order_id_), SPP_FIELD(next_fill_id_),
               SPP_FIELD(cash_));
};

} // namespace spp::quant::backtest

SPP_NAMED_RECORD(::spp::quant::backtest::TransactionCost, "TransactionCost",
                 SPP_FIELD(commission_per_share_), SPP_FIELD(commission_per_trade_),
                 SPP_FIELD(commission_pct_), SPP_FIELD(min_commission_),
                 SPP_FIELD(spread_pct_), SPP_FIELD(market_impact_));

SPP_NAMED_ENUM(::spp::quant::backtest::SlippageModel::Type, "SlippageType", Fixed,
               SPP_CASE(Fixed), SPP_CASE(Proportional), SPP_CASE(SquareRoot),
               SPP_CASE(AlmgrenChriss));

SPP_NAMED_RECORD(::spp::quant::backtest::SlippageModel, "SlippageModel",
                 SPP_FIELD(type_), SPP_FIELD(fixed_slippage_));
