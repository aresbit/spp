#pragma once

#include <spp/core/base.h>
#include <spp/containers/vec.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/numeric/math.h>
#include <spp/quant/strategy/types.h>

namespace spp::quant::backtest {

using spp::quant::strategy::Account_Snapshot;
using spp::quant::strategy::Trade;
using spp::quant::strategy::Market_Type;

// ---- Execution Mode ----
enum class Execution_Mode : u8 {
    research,       // fast, approximate fills, no strict constraints
    paper,          // realistic fills, standard constraints
    paper_strict,   // full exchange rule enforcement (limit up/down, T+1, etc.)
    live            // connected to live market
};

// ---- Slippage Model Type ----
enum class Slippage_Model_Type : u8 {
    fixed,        // price +/- absolute value
    percent,      // price * (1 +/- percentage)
    impact        // price * (1 +/- value * (trade_size/volume)^0.6)
};

// ---- Impact Model Type ----
enum class Impact_Model_Type : u8 {
    none_,             // no impact
    square_root,       // I = coeff * sigma * sqrt(Q/V)
    linear,            // I = coeff * sigma * participation_rate
    almgren_chriss     // permanent + temporary impact (Almgren-Chriss 2001)
};

// ---- Market Regime ----
enum class Regime : u8 {
    normal,
    bull,
    bear,
    high_volatility,
    low_volatility,
    crisis
};

// ---- Signal Scores (per-symbol) ----
struct Signal_Scores {
    f64 alpha_signal = 0.0;     // primary alpha factor score
    f64 momentum_score = 0.0;   // momentum factor score
    f64 value_score = 0.0;      // value factor score
    f64 quality_score = 0.0;    // quality factor score
    f64 volatility_score = 0.0; // volatility / risk factor score
    f64 composite = 0.0;        // weighted composite score
};

// ---- Cross-Sectional Rank Scores ----
struct Rank_Scores {
    f64 zscore = 0.0;           // cross-sectional z-score
    f64 percentile = 0.0;       // percentile rank [0, 1]
    f64 group_zscore = 0.0;     // within-group z-score
    f64 weighted_score = 0.0;   // final weighted composite for ranking
    String<Mdefault> group;     // industry/sector group label
};

// ---- Backtest Configuration ----
struct Backtest_Config {
    String<Mdefault> start_date;
    String<Mdefault> end_date;
    Execution_Mode mode = Execution_Mode::research;
    f64 init_cash = 100000.0;
    f64 commission_rate = 0.0003;    // per-trade fee (0.03% default)
    f64 slippage_value = 0.001;      // slippage parameter
    Slippage_Model_Type slippage_model = Slippage_Model_Type::percent;
    Impact_Model_Type impact_model = Impact_Model_Type::square_root;
    u32 portfolio_size = 10;         // max positions
    f64 rebalance_buffer = 0.02;     // min rebalance threshold (2%)
    u32 min_holding_bars = 5;         // min bars before sell
    f64 max_weight_per_position = 0.15; // max 15% per position
    f64 impact_coefficient = 0.1;    // impact model coefficient
    u32 t1_lock_bars = 0;            // T+N bars before sell allowed (A-share: 1 day)
};

// ---- Backtest Result ----
struct Backtest_Result {
    f64 total_return = 0.0;
    f64 annual_return = 0.0;
    f64 max_drawdown = 0.0;
    f64 sharpe_ratio = 0.0;
    f64 calmar_ratio = 0.0;
    f64 sortino_ratio = 0.0;
    f64 final_equity = 0.0;
    Vec<Account_Snapshot, Mdefault> equity_curve;
    Vec<Trade, Mdefault> trades;
    Vec<String<Mdefault>, Mdefault> rejected_orders;
    f64 total_commission = 0.0;
    f64 total_slippage_cost = 0.0;
    f64 win_rate = 0.0;
    f64 avg_win_loss_ratio = 0.0;
};

} // namespace spp::quant::backtest

// ============================================================
// Reflection specializations (must be in namespace spp)
// ============================================================

namespace spp {

SPP_NAMED_ENUM(::spp::quant::backtest::Execution_Mode, "Execution_Mode",
    research,
    SPP_CASE(research), SPP_CASE(paper), SPP_CASE(paper_strict), SPP_CASE(live));

SPP_NAMED_ENUM(::spp::quant::backtest::Slippage_Model_Type, "Slippage_Model_Type",
    fixed,
    SPP_CASE(fixed), SPP_CASE(percent), SPP_CASE(impact));

SPP_NAMED_ENUM(::spp::quant::backtest::Impact_Model_Type, "Impact_Model_Type",
    none_,
    SPP_CASE(none_), SPP_CASE(square_root), SPP_CASE(linear), SPP_CASE(almgren_chriss));

SPP_NAMED_ENUM(::spp::quant::backtest::Regime, "Regime",
    normal,
    SPP_CASE(normal), SPP_CASE(bull), SPP_CASE(bear),
    SPP_CASE(high_volatility), SPP_CASE(low_volatility), SPP_CASE(crisis));

SPP_NAMED_RECORD(::spp::quant::backtest::Signal_Scores, "Signal_Scores",
    SPP_FIELD(alpha_signal),
    SPP_FIELD(momentum_score),
    SPP_FIELD(value_score),
    SPP_FIELD(quality_score),
    SPP_FIELD(volatility_score),
    SPP_FIELD(composite));

SPP_NAMED_RECORD(::spp::quant::backtest::Rank_Scores, "Rank_Scores",
    SPP_FIELD(zscore),
    SPP_FIELD(percentile),
    SPP_FIELD(group_zscore),
    SPP_FIELD(weighted_score),
    SPP_FIELD(group));

SPP_NAMED_RECORD(::spp::quant::backtest::Backtest_Config, "Backtest_Config",
    SPP_FIELD(start_date),
    SPP_FIELD(end_date),
    SPP_FIELD(mode),
    SPP_FIELD(init_cash),
    SPP_FIELD(commission_rate),
    SPP_FIELD(slippage_value),
    SPP_FIELD(slippage_model),
    SPP_FIELD(impact_model),
    SPP_FIELD(portfolio_size),
    SPP_FIELD(rebalance_buffer),
    SPP_FIELD(min_holding_bars),
    SPP_FIELD(max_weight_per_position),
    SPP_FIELD(impact_coefficient),
    SPP_FIELD(t1_lock_bars));

SPP_NAMED_RECORD(::spp::quant::backtest::Backtest_Result, "Backtest_Result",
    SPP_FIELD(total_return),
    SPP_FIELD(annual_return),
    SPP_FIELD(max_drawdown),
    SPP_FIELD(sharpe_ratio),
    SPP_FIELD(calmar_ratio),
    SPP_FIELD(sortino_ratio),
    SPP_FIELD(final_equity),
    SPP_FIELD(equity_curve),
    SPP_FIELD(trades),
    SPP_FIELD(rejected_orders),
    SPP_FIELD(total_commission),
    SPP_FIELD(total_slippage_cost),
    SPP_FIELD(win_rate),
    SPP_FIELD(avg_win_loss_ratio));

} // namespace spp
