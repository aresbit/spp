#pragma once

#include <spp/core/base.h>
#include <spp/quant/risk/risk_config.h>

namespace spp::quant::risk {

struct Order {
    String<Mdefault> order_id;
    String<Mdefault> symbol;
    f64 price;
    f64 quantity;
    u8 side;
    Deterministic_Time timestamp;
};

struct Position {
    String<Mdefault> symbol;
    f64 quantity;
    f64 avg_price;
    f64 market_price;
};

struct Trade {
    String<Mdefault> order_id;
    String<Mdefault> symbol;
    f64 price;
    f64 quantity;
    f64 pnl;
    f64 commission;
    Deterministic_Time timestamp;
};

template <typename A = Mdefault>
struct Risk_Checker {
    Risk_Config config;
    Risk_State state;

    Risk_Checker() noexcept = default;
    explicit Risk_Checker(const Risk_Config& cfg) noexcept : config(cfg) {}

    auto check_order(const Order& order, f64 current_equity,
                      const Vec<Position, A>& positions) -> Result<u8, String_View> {
        if (state.halted) return Result<u8, String_View>::err("trading_halted"_v);

        f64 order_value = Math::abs(order.price * order.quantity);
        if (order_value > config.max_single_order_value)
            return Result<u8, String_View>::err("max_single_order_value_exceeded"_v);

        if (state.daily_trades >= config.max_daily_trades)
            return Result<u8, String_View>::err("max_daily_trades_exceeded"_v);

        f64 eq_abs = Math::abs(current_equity);
        if (eq_abs > 1e-12) {
            f64 total_gross = 0.0, total_net = 0.0;
            for (u64 i = 0; i < positions.length(); i++) {
                f64 pv = Math::abs(positions[i].market_price * positions[i].quantity);
                total_gross += pv;
                total_net += positions[i].market_price * positions[i].quantity;
            }

            if (order_value / eq_abs > config.max_position_weight)
                return Result<u8, String_View>::err("max_position_weight_exceeded"_v);

            if ((total_gross + order_value) / eq_abs > config.max_gross_exposure)
                return Result<u8, String_View>::err("max_gross_exposure_exceeded"_v);

            f64 new_net = (total_net + (order.side == 0 ? order_value : -order_value))
                          / eq_abs;
            if (Math::abs(new_net) > config.max_net_exposure)
                return Result<u8, String_View>::err("max_net_exposure_exceeded"_v);

            f64 ds = Math::abs(state.daily_start_equity);
            // Daily LOSS limit: only a negative daily PnL counts.  Using
            // abs() here would halt a *profitable* session once gains grew
            // past the threshold.
            if (ds > 1e-12 && state.daily_pnl < 0.0
                && (-state.daily_pnl) / ds > config.daily_loss_limit_pct)
                return Result<u8, String_View>::err("daily_loss_limit_exceeded"_v);
        }

        if (state.current_drawdown > config.max_drawdown_pct)
            return Result<u8, String_View>::err("max_drawdown_exceeded"_v);

        return Result<u8, String_View>::ok(u8{0});
    }

    auto check_portfolio(const Vec<Position, A>& positions, f64 equity)
        -> Result<u8, String_View> {
        if (state.halted) return Result<u8, String_View>::err("trading_halted"_v);
        if (Math::abs(equity) < 1e-12) return Result<u8, String_View>::ok(u8{0});

        f64 gross = 0.0, net = 0.0;
        for (u64 i = 0; i < positions.length(); i++) {
            f64 pv = Math::abs(positions[i].market_price * positions[i].quantity);
            gross += pv;
            net += positions[i].market_price * positions[i].quantity;
        }

        if (gross / equity > config.max_gross_exposure)
            return Result<u8, String_View>::err("portfolio_gross_exposure_exceeded"_v);
        if (Math::abs(net / equity) > config.max_net_exposure)
            return Result<u8, String_View>::err("portfolio_net_exposure_exceeded"_v);

        return Result<u8, String_View>::ok(u8{0});
    }

    auto new_day(f64 equity) -> void {
        state.daily_start_equity = equity;
        state.daily_trades = 0;
        state.daily_pnl = 0.0;
        state.trade_pnls = Vec<f64, Mdefault>{};
        state.trade_pnls.reserve(config.max_daily_trades);
        state.halted = false;
    }

    auto record_trade(const Trade& trade, f64 equity) -> void {
        state.daily_trades++;
        state.daily_pnl += trade.pnl;
        state.trade_pnls.push(trade.pnl);

        f64 ds = Math::abs(state.daily_start_equity);
        // Only a net loss for the day breaches the daily-loss limit.
        if (ds > 1e-12 && state.daily_pnl < 0.0
            && (-state.daily_pnl) / ds > config.daily_loss_limit_pct) {
            state.halted = true;
            state.violation_log.push("daily_loss_limit_breached"_v.string<Mdefault>());
        }

        if (Math::abs(equity) > 1e-12 && trade.pnl < 0.0) {
            f64 tl = Math::abs(trade.pnl) / Math::abs(equity);
            if (tl > config.single_trade_loss_limit)
                state.violation_log.push("single_trade_loss_exceeded"_v.string<Mdefault>());
        }

        update_drawdown(equity);
    }

    auto update_drawdown(f64 current_equity) -> void {
        if (current_equity > state.peak_equity) state.peak_equity = current_equity;
        f64 pa = Math::abs(state.peak_equity);
        if (pa > 1e-12) {
            state.current_drawdown = (state.peak_equity - current_equity) / pa;
            if (state.current_drawdown > config.max_drawdown_pct) {
                state.halted = true;
                state.violation_log.push("max_drawdown_breached"_v.string<Mdefault>());
            }
        }
    }
};

} // namespace spp::quant::risk
