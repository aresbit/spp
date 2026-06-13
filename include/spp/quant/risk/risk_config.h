#pragma once

#include <spp/core/base.h>

namespace spp::quant::risk {

struct Risk_Config {
    f64 max_position_weight = 0.20;
    f64 max_gross_exposure = 1.0;
    f64 max_net_exposure = 1.0;
    f64 max_single_order_value = 1000000.0;
    u32 max_daily_trades = 100;
    f64 max_drawdown_pct = 0.15;
    f64 daily_loss_limit_pct = 0.05;
    f64 single_trade_loss_limit = 0.03;
    f64 max_sector_concentration = 0.40;
    u32 max_correlated_positions = 3;
    f64 max_portfolio_volatility = 0.30;
    f64 var_confidence = 0.95;
    u32 var_horizon = 1;
};

struct Risk_State {
    f64 peak_equity = 0.0;
    f64 daily_start_equity = 0.0;
    u32 daily_trades = 0;
    f64 daily_pnl = 0.0;
    f64 current_drawdown = 0.0;
    Vec<f64, Mdefault> trade_pnls;
    Vec<String<Mdefault>, Mdefault> violation_log;
    bool halted = false;
};

} // namespace spp::quant::risk

SPP_RECORD(spp::quant::risk::Risk_Config,
    SPP_FIELD(max_position_weight), SPP_FIELD(max_gross_exposure),
    SPP_FIELD(max_net_exposure), SPP_FIELD(max_single_order_value),
    SPP_FIELD(max_daily_trades), SPP_FIELD(max_drawdown_pct),
    SPP_FIELD(daily_loss_limit_pct), SPP_FIELD(single_trade_loss_limit),
    SPP_FIELD(max_sector_concentration), SPP_FIELD(max_correlated_positions),
    SPP_FIELD(max_portfolio_volatility), SPP_FIELD(var_confidence),
    SPP_FIELD(var_horizon));

SPP_RECORD(spp::quant::risk::Risk_State,
    SPP_FIELD(peak_equity), SPP_FIELD(daily_start_equity),
    SPP_FIELD(daily_trades), SPP_FIELD(daily_pnl), SPP_FIELD(current_drawdown),
    SPP_FIELD(trade_pnls), SPP_FIELD(violation_log), SPP_FIELD(halted));
