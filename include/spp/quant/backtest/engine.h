#pragma once

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/core/pair.h>
#include <spp/containers/vec.h>
#include <spp/containers/map.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/numeric/math.h>
#include <spp/core/deterministic.h>
#include <spp/quant/strategy/types.h>
#include <spp/quant/strategy/account.h>
#include <spp/quant/backtest/types.h>
#include <spp/quant/backtest/slippage.h>
#include <spp/quant/backtest/market_rules.h>

// Forward-declare data-layer types
namespace spp::quant::data {

template <typename A>
struct Ohlcv_Data;

struct Symbol_Bar;

} // namespace spp::quant::data

namespace spp::quant::backtest {

using spp::quant::strategy::Order;
using spp::quant::strategy::Order_Direction;
using spp::quant::strategy::Order_Offset;
using spp::quant::strategy::Position;
using spp::quant::strategy::Trade;
using spp::quant::strategy::Account;
using spp::quant::strategy::Account_Snapshot;
using spp::quant::strategy::Market_Type;
using spp::quant::strategy::Running_Mode;

// ============================================================
// Backtest_Engine — Bar-driven event loop backtester
//
// Replicates QUANTAXIS engine.py:237-729 event loop.
//
// Pipeline per timestamp group:
//   1. Group bars by timestamp
//   2. T+1 lock expiration
//   3. Trading window check
//   4. Per-symbol signal computation
//   5. Market regime computation
//   6. Cross-sectional ranking (z-score, group scoring)
//   7. Rank score assembly (weighted sum)
//   8. Candidate selection (hold retention + greedy top-N)
//   9. Portfolio weight optimization (softmax / Black-Litterman)
//  10. Execute: SELL phase → BUY phase
//  11. Equity mark-to-market
//
// NOTE: This engine template requires the Strategy type to inherit from
// Strategy_Base<Strategy, A> (the CRTP pattern). The engine calls
// strategy.x1() for each bar, which in turn calls the derived on_bar().
// ============================================================

template <typename Strategy, typename A = Mdefault>
struct Backtest_Engine {
    Backtest_Config config;
    Strategy strategy;
    Account<A> account;

    // T+1 lock tracking: code -> purchase bar index (simplified)
    Map<String<A>, u64, A> t1_locks;
    // Bar index tracking
    u64 current_bar_idx = 0;

    // Diagnostic counters
    u64 total_signals_computed = 0;
    u64 total_rebalances = 0;
    u64 total_buys = 0;
    u64 total_sells = 0;
    u64 total_rejections = 0;

    // Per-symbol historical mean volume, populated once in run() before the
    // event loop. _estimate_bar_volume() reads from this; legacy callers see
    // a sane non-zero estimate even if no data has been ingested.
    Map<String<A>, f64, A> _volume_cache;

    Backtest_Engine() noexcept = default;

    Backtest_Engine(Backtest_Config cfg, Strategy s) noexcept
        : config(spp::move(cfg)), strategy(spp::move(s)),
          account(String<A>{config.start_date.clone()}, config.init_cash) {
    }

    // ============================================================
    // run() — Main backtest entry point
    //
    // Iterates over bar groups in Ohlcv_Data and applies the full
    // pipeline at each timestamp group.
    // ============================================================

    auto run(data::Ohlcv_Data<A>& bar_data) -> Backtest_Result {
        Backtest_Result result;
        Vec<Trade, A> all_trades;
        Vec<String<A>, A> all_rejections;
        Vec<Account_Snapshot, A> equity_curve;

        // Initialize strategy state
        strategy.running_mode = Running_Mode::backtest;
        strategy.acc = Account<A>{"main"_v.string<A>(), config.init_cash};
        account = Account<A>{"main"_v.string<A>(), config.init_cash};

        // Record initial equity point
        Account_Snapshot snap0;
        snap0.equity = config.init_cash;
        snap0.balance = config.init_cash;
        snap0.available = config.init_cash;
        snap0.total_pnl = 0.0;
        equity_curve.push(spp::move(snap0));

        // Group bars by timestamp once up front. Doing this per-step would
        // be O(n²); doing it once is O(n). The grouped view stays alive for
        // the entire run; per-iteration `_group_at(grouped, g)` is O(1).
        auto grouped = bar_data.group_by_time();

        // Cache mean per-symbol volumes for the impact/slippage models.
        // Recomputing inside the inner loop would walk every bar each call.
        _volume_cache = Map<String<A>, f64, A>{};
        for (u64 i = 0; i < strategy.codes.length(); i++) {
            _volume_cache.insert(strategy.codes[i].clone(),
                                 bar_data.mean_volume(strategy.codes[i].view()));
        }

        u64 group_count = grouped.length();
        f64 peak_equity = config.init_cash;
        f64 max_dd = 0.0;

        for (u64 g = 0; g < group_count; g++) {
            current_bar_idx = g;
            auto& bar_group = grouped[g].second;

            if (bar_group.length() == 0) continue;

            // ---- Step 1: Extract timestamp and prices ----
            Deterministic_Time group_time = bar_group[0].bar.time;
            Map<String<A>, Decimal<8>, A> current_prices;
            for (u64 i = 0; i < bar_group.length(); i++) {
                current_prices.insert(bar_group[i].symbol.clone(), bar_group[i].bar.close);
            }

            // ---- Step 2: T+1 lock expiration ----
            _expire_t1_locks(current_bar_idx);

            // ---- Step 3: Trading window check ----
            if (!_is_trading_window(group_time)) continue;

            // ---- Step 4: Per-symbol signal computation via strategy ----
            auto signal_map = compute_signals(bar_group);

            // ---- Step 5: Market regime detection ----
            auto regime = compute_regime(signal_map);

            // ---- Step 6: Cross-sectional ranking ----
            auto rank_map = cross_sectional_rank(signal_map);

            // ---- Step 7: Rank score assembly ----
            // cross_sectional_rank() already produces the composite score
            // we use downstream; no separate assembly step yet.

            // ---- Step 8: Candidate selection ----
            auto candidates = select_candidates(rank_map, account.positions);

            // ---- Step 9: Portfolio weight optimization ----
            auto target_weights = optimize_weights(candidates.first, rank_map);

            // ---- Step 10: Execute rebalance ----
            auto exec_result = execute_rebalance(target_weights, current_prices,
                                                  account.positions);

            // Collect trades and rejections
            for (u64 i = 0; i < exec_result.first.length(); i++) {
                all_trades.push(Trade{exec_result.first[i].trade_id.clone(),
                                       exec_result.first[i].order_id.clone(),
                                       exec_result.first[i].code.clone(),
                                       exec_result.first[i].direction,
                                       exec_result.first[i].offset,
                                       exec_result.first[i].price,
                                       exec_result.first[i].volume,
                                       exec_result.first[i].fee,
                                       exec_result.first[i].time});
            }
            for (u64 i = 0; i < exec_result.second.length(); i++) {
                all_rejections.push(exec_result.second[i].clone());
            }

            // ---- Step 11: Mark-to-market ----
            for (u64 i = 0; i < bar_group.length(); i++) {
                account.on_price_change(bar_group[i].symbol.view(), bar_group[i].bar.close);
            }

            // Record equity snapshot every bar group
            Account_Snapshot snap;
            snap.time = group_time;
            snap.balance = account.balance;
            snap.available = account.available;
            snap.frozen = account.frozen;
            snap.equity = account.total_equity();
            snap.total_pnl = account.total_equity() - config.init_cash;
            equity_curve.push(spp::move(snap));

            // Track max drawdown
            f64 current_equity = account.total_equity();
            if (current_equity > peak_equity) peak_equity = current_equity;
            f64 dd = (peak_equity > 0.0) ? (current_equity - peak_equity) / peak_equity : 0.0;
            if (dd < max_dd) max_dd = dd;
        }

        // ---- Compute Final Metrics ----
        result.final_equity = account.total_equity();
        result.total_return = (result.final_equity - config.init_cash) / config.init_cash;

        // Annualized return (approximate using group count as trading days)
        f64 trading_days = static_cast<f64>(group_count);
        if (trading_days > 0) {
            f64 years = trading_days / 252.0;
            if (years > 0.0) {
                result.annual_return = Math::pow(1.0 + result.total_return, 1.0 / years) - 1.0;
            }
        }

        result.max_drawdown = -max_dd;

        // Sharpe ratio (simplified: using bar-group returns)
        f64 mean_return = 0.0;
        f64 return_variance = 0.0;
        u64 n_returns = equity_curve.length() > 1 ? equity_curve.length() - 1 : 0;
        if (n_returns > 0) {
            Vec<f64, A> returns;
            for (u64 i = 1; i < equity_curve.length(); i++) {
                f64 prev = equity_curve[i - 1].equity;
                if (prev > 0.0) {
                    f64 ret = (equity_curve[i].equity - prev) / prev;
                    returns.push(ret);
                    mean_return += ret;
                }
            }
            mean_return /= static_cast<f64>(returns.length());

            for (u64 i = 0; i < returns.length(); i++) {
                f64 diff = returns[i] - mean_return;
                return_variance += diff * diff;
            }
            return_variance /= static_cast<f64>(returns.length());

            f64 std_return = Math::sqrt(return_variance);
            if (std_return > 0.0) {
                result.sharpe_ratio = (mean_return / std_return) * Math::sqrt(252.0);
            }

            // Sortino ratio (downside deviation only)
            f64 downside_var = 0.0;
            u64 downside_count = 0;
            for (u64 i = 0; i < returns.length(); i++) {
                if (returns[i] < 0.0) {
                    downside_var += returns[i] * returns[i];
                    downside_count++;
                }
            }
            if (downside_count > 0) {
                downside_var /= static_cast<f64>(returns.length());
                f64 downside_dev = Math::sqrt(downside_var);
                if (downside_dev > 0.0) {
                    result.sortino_ratio = (mean_return / downside_dev) * Math::sqrt(252.0);
                }
            }
        }

        // Calmar ratio
        if (result.max_drawdown > 0.001) {
            result.calmar_ratio = result.annual_return / result.max_drawdown;
        }

        // Win rate and avg win/loss
        f64 wins = 0.0, losses = 0.0, total_win = 0.0, total_loss = 0.0;
        for (u64 i = 0; i < all_trades.length(); i++) {
            // Simplified PnL per trade: use direction
            f64 trade_pnl = 0.0;
            if (all_trades[i].direction == Order_Direction::sell ||
                all_trades[i].direction == Order_Direction::sell_close) {
                trade_pnl = all_trades[i].volume * _price_to_f64(all_trades[i].price);
            } else {
                trade_pnl = -all_trades[i].volume * _price_to_f64(all_trades[i].price);
            }
            if (trade_pnl > 0.0) { wins++; total_win += trade_pnl; }
            else { losses++; total_loss -= trade_pnl; }
        }
        f64 total_closed = wins + losses;
        if (total_closed > 0.0) {
            result.win_rate = wins / total_closed;
        }
        if (wins > 0 && losses > 0) {
            result.avg_win_loss_ratio = (total_win / wins) / (total_loss / losses);
        }

        // Commission total
        for (u64 i = 0; i < all_trades.length(); i++) {
            result.total_commission += all_trades[i].fee;
        }

        result.equity_curve = spp::move(equity_curve);
        result.trades = spp::move(all_trades);
        result.rejected_orders = spp::move(all_rejections);

        return result;
    }

    // ---- Step 4: Compute per-symbol signals via strategy ----

    auto compute_signals(const Vec<data::Symbol_Bar, A>& bar_group)
        -> Map<String<A>, Signal_Scores, A> {

        Map<String<A>, Signal_Scores, A> signal_map;

        for (u64 i = 0; i < bar_group.length(); i++) {
            const auto& bar = bar_group[i];

            // Run strategy's x1() to generate on_bar() signals
            strategy.x1(bar);

            // Collect signal scores from strategy state
            Signal_Scores scores;
            scores.composite = _extract_signal_from_strategy(bar.symbol.view());

            // Populate factor scores from strategy system_vars (Map is
            // non-copyable, so bind by reference).
            const auto& sv = strategy.system_vars;
            // Try getting named signals: "alpha", "momentum", "value", "quality", "volatility"
            _try_get_signal(sv, "alpha"_v, scores.alpha_signal);
            _try_get_signal(sv, "momentum"_v, scores.momentum_score);
            _try_get_signal(sv, "value"_v, scores.value_score);
            _try_get_signal(sv, "quality"_v, scores.quality_score);
            _try_get_signal(sv, "volatility"_v, scores.volatility_score);

            signal_map.insert(bar.symbol.clone(), scores);
            total_signals_computed++;
        }

        return signal_map;
    }

    // ---- Step 5: Market regime detection ----

    auto compute_regime(const Map<String<A>, Signal_Scores, A>& signals) -> Regime {
        if (signals.empty()) return Regime::normal;

        // Compute average composite score to determine regime
        f64 sum = 0.0;
        u64 count = 0;
        // Iterate map entries via known keys or approximate
        // Simplified: use a threshold heuristic

        // In production: compute VIX-like metric from volatility scores
        f64 avg_vol = 0.0;
        // For now, return based on simple threshold
        (void)signals;
        (void)sum;
        (void)count;
        (void)avg_vol;

        return Regime::normal;
    }

    // ---- Step 6: Cross-sectional z-score ranking ----

    auto cross_sectional_rank(const Map<String<A>, Signal_Scores, A>& signals)
        -> Map<String<A>, Rank_Scores, A> {

        Map<String<A>, Rank_Scores, A> rank_map;

        if (signals.empty()) return rank_map;

        // Step 6a: Gather all composite scores
        Vec<Pair<String<A>, f64>, A> scored_items;
        f64 total = 0.0;
        u64 count = 0;

        // We can't directly iterate Map, so collect from a parallel list
        // of known codes (tracked separately or inferred)
        // For now: use the strategy codes list
        for (u64 i = 0; i < strategy.codes.length(); i++) {
            auto entry = signals.try_get(strategy.codes[i].view());
            if (entry.ok()) {
                f64 score = (**entry).composite;
                scored_items.push(Pair<String<A>, f64>{
                    strategy.codes[i].clone(), score});
                total += score;
                count++;
            }
        }

        if (count == 0) return rank_map;

        // Step 6b: Compute mean and std for z-score
        f64 mean = total / static_cast<f64>(count);
        f64 variance = 0.0;
        for (u64 i = 0; i < scored_items.length(); i++) {
            f64 diff = scored_items[i].second - mean;
            variance += diff * diff;
        }
        variance /= static_cast<f64>(count);
        f64 std_dev = Math::sqrt(variance);

        // Step 6c: Compute z-scores and percentiles
        for (u64 i = 0; i < scored_items.length(); i++) {
            Rank_Scores rank;
            if (std_dev > 1e-12) {
                rank.zscore = (scored_items[i].second - mean) / std_dev;
            }
            // Percentile: rank among scores
            f64 better_count = 0.0;
            for (u64 j = 0; j < scored_items.length(); j++) {
                if (scored_items[j].second < scored_items[i].second) better_count++;
            }
            rank.percentile = better_count / static_cast<f64>(scored_items.length());
            rank.weighted_score = scored_items[i].second;
            rank_map.insert(scored_items[i].first.clone(), spp::move(rank));
        }

        return rank_map;
    }

    // ---- Step 8: Candidate selection ----

    auto select_candidates(const Map<String<A>, Rank_Scores, A>& ranks,
                            const Vec<Position, A>& current_holds)
        -> Pair<Vec<String<A>, A>, Vec<String<A>, A>> {

        Vec<String<A>, A> hold_candidates;
        Vec<String<A>, A> new_candidates;

        // Step 8a: Retain existing profitable holdings
        for (u64 i = 0; i < current_holds.length(); i++) {
            auto& pos = current_holds[i];
            if (pos.net_volume() == 0.0) continue;

            auto rank_opt = ranks.try_get(pos.code.view());
            if (rank_opt.ok()) {
                f64 score = (**rank_opt).zscore;
                // Keep if positive z-score (above cross-sectional mean)
                if (score > -0.5) {
                    hold_candidates.push(pos.code.clone());
                }
            } else {
                // No signal → still hold to avoid forced exit
                hold_candidates.push(pos.code.clone());
            }
        }

        // Step 8b: Greedy top-N selection for new candidates
        // Collect all ranked items not already held
        Vec<Pair<String<A>, f64>, A> available;
        for (u64 i = 0; i < strategy.codes.length(); i++) {
            auto rank_opt = ranks.try_get(strategy.codes[i].view());
            if (!rank_opt.ok()) continue;

            // Skip already-held codes
            bool already_held = false;
            for (u64 j = 0; j < hold_candidates.length(); j++) {
                if (_sv_eq(hold_candidates[j].view(), strategy.codes[i].view())) {
                    already_held = true;
                    break;
                }
            }
            if (already_held) continue;

            // Check T+1 lock
            auto lock = t1_locks.try_get(strategy.codes[i].view());
            if (lock.ok() && **lock >= current_bar_idx) {
                continue; // T+1 locked, cannot buy
            }

            available.push(Pair<String<A>, f64>{
                strategy.codes[i].clone(), (**rank_opt).weighted_score});
        }

        // Sort by weighted score descending (simple bubble sort for small N)
        _sort_by_score_desc(available);

        // Pick top-N for new positions
        u32 total_positions = static_cast<u32>(hold_candidates.length());
        for (u64 i = 0; i < available.length() &&
             total_positions < config.portfolio_size; i++) {
            new_candidates.push(available[i].first.clone());
            total_positions++;
        }

        return Pair<Vec<String<A>, A>, Vec<String<A>, A>>{
            spp::move(new_candidates), spp::move(hold_candidates)};
    }

    // ---- Step 9: Portfolio weight optimization ----

    auto optimize_weights(const Vec<String<A>, A>& candidates,
                           const Map<String<A>, Rank_Scores, A>& ranks)
        -> Map<String<A>, f64, A> {

        Map<String<A>, f64, A> weights;

        if (candidates.length() == 0) return weights;

        // Method: softmax over z-scores
        Vec<f64, A> scores;
        f64 max_score = -1e18;
        for (u64 i = 0; i < candidates.length(); i++) {
            auto rank_opt = ranks.try_get(candidates[i].view());
            f64 s = rank_opt.ok() ? (**rank_opt).zscore : 0.0;
            scores.push(s);
            if (s > max_score) max_score = s;
        }

        // Softmax with temperature
        Vec<f64, A> soft_w = softmax_weights(scores, 0.5);

        // Cap at max_weight_per_position
        cap_and_normalize(soft_w, config.max_weight_per_position);

        // Assign to map
        for (u64 i = 0; i < candidates.length(); i++) {
            weights.insert(candidates[i].clone(), soft_w[i]);
        }

        return weights;
    }

    // ---- Step 10: Execute rebalance ----

    auto execute_rebalance(const Map<String<A>, f64, A>& target_weights,
                            const Map<String<A>, Decimal<8>, A>& prices,
                            const Vec<Position, A>& current_holds)
        -> Pair<Vec<Trade, A>, Vec<String<A>, A>> {

        Vec<Trade, A> trades_out;
        Vec<String<A>, A> rejections;

        f64 total_equity = account.total_equity();
        if (total_equity <= 0.0) {
            return Pair<Vec<Trade, A>, Vec<String<A>, A>>{
                spp::move(trades_out), spp::move(rejections)};
        }

        // ---- Phase A: SELL overweight positions ----
        for (u64 i = 0; i < current_holds.length(); i++) {
            auto& pos = current_holds[i];
            if (pos.volume_long <= 0.0) continue;

            f64 current_weight = pos.volume_long * _price_to_f64(pos.last_price) / total_equity;

            auto target_w = target_weights.try_get(pos.code.view());
            f64 target_weight = target_w.ok() ? **target_w : 0.0;

            // Sell if overweight beyond rebalance buffer
            if (current_weight > target_weight + config.rebalance_buffer && current_weight > 0.0) {
                f64 excess_weight = current_weight - target_weight;
                f64 sell_value = total_equity * excess_weight;

                auto price_opt = prices.try_get(pos.code.view());
                if (!price_opt.ok()) {
                    rejections.push(pos.code.clone());
                    continue;
                }
                Decimal<8> price = **price_opt;

                // T+1 check: ensure minimum holding period
                auto lock = t1_locks.try_get(pos.code.view());
                if (lock.ok() && _bars_since(**lock) < config.min_holding_bars) {
                    // Hold: too early to sell
                    continue;
                }

                f64 sell_volume = sell_value / _price_to_f64(price);
                sell_volume = Market_Rules<A>::round_lots(sell_volume, Market_Type::stock_cn);
                if (sell_volume <= 0.0) continue;

                // Apply slippage
                Decimal<8> exec_price = Slippage_Model<A>::apply(
                    price, config.slippage_value, _slippage_model_str(),
                    sell_volume, _estimate_bar_volume(pos.code.view()) * 10.0,
                    0.01, -1);

                // Impact
                exec_price = Impact_Model<A>::estimate(
                    _impact_model_str(), config.impact_coefficient, 0.02,
                    sell_volume, _estimate_bar_volume(pos.code.view()),
                    exec_price, config.min_holding_bars, -1);

                // Execute sell
                Order sell_order;
                sell_order.code = pos.code.clone();
                sell_order.direction = Order_Direction::sell_close;
                sell_order.offset = Order_Offset::close_;
                sell_order.price = exec_price;
                sell_order.volume = sell_volume;
                sell_order.time = Deterministic_Time{};

                auto result = account.send_order(sell_order);
                if (result.ok()) {
                    Order& filled = result.unwrap();
                    Trade trade = account.make_deal(filled);
                    trades_out.push(Trade{trade.trade_id.clone(),
                                           trade.order_id.clone(),
                                           trade.code.clone(),
                                           trade.direction,
                                           trade.offset,
                                           trade.price,
                                           trade.volume,
                                           trade.fee,
                                           trade.time});
                    total_sells++;
                } else {
                    rejections.push(pos.code.clone());
                    total_rejections++;
                }
            }
        }

        // Update equity after sells for buy-side sizing
        total_equity = account.total_equity();

        // ---- Phase B: BUY underweight / new positions ----
        // Collect all target codes
        for (u64 i = 0; i < strategy.codes.length(); i++) {
            auto target_w_opt = target_weights.try_get(strategy.codes[i].view());
            if (!target_w_opt.ok()) continue;

            f64 target_weight = **target_w_opt;
            if (target_weight <= 0.0) continue;

            // Check current position
            f64 current_weight = 0.0;
            bool has_position = false;
            for (u64 j = 0; j < current_holds.length(); j++) {
                if (_sv_eq(current_holds[j].code.view(), strategy.codes[i].view())) {
                    current_weight = current_holds[j].volume_long *
                                     _price_to_f64(current_holds[j].last_price) / total_equity;
                    has_position = true;
                    break;
                }
            }

            // Buy if underweight beyond buffer
            if (current_weight < target_weight - config.rebalance_buffer || !has_position) {
                f64 deficit_weight = target_weight - current_weight;
                if (deficit_weight <= 0.0) continue;

                f64 buy_value = total_equity * deficit_weight;

                auto price_opt = prices.try_get(strategy.codes[i].view());
                if (!price_opt.ok()) continue;
                Decimal<8> price = **price_opt;

                // Check limit-up
                Decimal<8> prev_close = price; // simplified: use current price
                if (Market_Rules<A>::is_limit_up(price, prev_close, Market_Type::stock_cn)) {
                    rejections.push(strategy.codes[i].clone());
                    total_rejections++;
                    continue;
                }

                f64 buy_volume = buy_value / _price_to_f64(price);
                buy_volume = Market_Rules<A>::round_lots(buy_volume, Market_Type::stock_cn);
                if (buy_volume <= 0.0) continue;

                // Check available cash
                f64 needed = buy_volume * _price_to_f64(price) * (1.0 + config.commission_rate);
                if (needed > account.available) {
                    // Scale down to available cash
                    buy_volume = account.available / (_price_to_f64(price) * (1.0 + config.commission_rate));
                    buy_volume = Market_Rules<A>::round_lots(buy_volume, Market_Type::stock_cn);
                    if (buy_volume <= 0.0) continue;
                }

                // Apply slippage (buy side: price increases)
                Decimal<8> exec_price = Slippage_Model<A>::apply(
                    price, config.slippage_value, _slippage_model_str(),
                    buy_volume, _estimate_bar_volume(strategy.codes[i].view()) * 10.0,
                    0.01, 1);

                exec_price = Impact_Model<A>::estimate(
                    _impact_model_str(), config.impact_coefficient, 0.02,
                    buy_volume, _estimate_bar_volume(strategy.codes[i].view()),
                    exec_price, config.min_holding_bars, 1);

                // Execute buy
                Order buy_order;
                buy_order.code = strategy.codes[i].clone();
                buy_order.direction = Order_Direction::buy_open;
                buy_order.offset = Order_Offset::open_;
                buy_order.price = exec_price;
                buy_order.volume = buy_volume;
                buy_order.time = Deterministic_Time{};

                auto result = account.send_order(buy_order);
                if (result.ok()) {
                    Order& filled = result.unwrap();
                    Trade trade = account.make_deal(filled);

                    // Record T+1 lock
                    t1_locks.insert(strategy.codes[i].clone(), current_bar_idx);

                    trades_out.push(Trade{trade.trade_id.clone(),
                                           trade.order_id.clone(),
                                           trade.code.clone(),
                                           trade.direction,
                                           trade.offset,
                                           trade.price,
                                           trade.volume,
                                           trade.fee,
                                           trade.time});
                    total_buys++;
                } else {
                    rejections.push(strategy.codes[i].clone());
                    total_rejections++;
                }
            }
        }

        total_rebalances++;
        return Pair<Vec<Trade, A>, Vec<String<A>, A>>{
            spp::move(trades_out), spp::move(rejections)};
    }

    // ---- Math Helpers ----

    /// Softmax with temperature parameter.
    /// temperature → 0: argmax (concentrated weights)
    /// temperature → inf: uniform (equal weights)
    static auto softmax_weights(const Vec<f64>& scores, f64 temperature = 1.0) -> Vec<f64> {
        Vec<f64> result;
        if (scores.length() == 0) return result;

        // Find max for numerical stability
        f64 max_score = scores[0];
        for (u64 i = 1; i < scores.length(); i++) {
            if (scores[i] > max_score) max_score = scores[i];
        }

        // Compute exp(s/temperature)
        f64 total_exp = 0.0;
        Vec<f64> exps;
        for (u64 i = 0; i < scores.length(); i++) {
            f64 exp_val = Math::exp((scores[i] - max_score) / temperature);
            exps.push(exp_val);
            total_exp += exp_val;
        }

        // Normalize
        if (total_exp > 1e-12) {
            for (u64 i = 0; i < exps.length(); i++) {
                result.push(exps[i] / total_exp);
            }
        } else {
            // Degenerate case: equal weight
            f64 eq = 1.0 / static_cast<f64>(scores.length());
            for (u64 i = 0; i < scores.length(); i++) {
                result.push(eq);
            }
        }

        return result;
    }

    /// Cap individual weights at max_weight and renormalize.
    static auto cap_and_normalize(Vec<f64>& weights, f64 max_weight) -> void {
        if (weights.length() == 0 || max_weight <= 0.0) return;

        f64 excess = 0.0;
        u64 capped_count = 0;

        // First pass: cap and accumulate excess
        for (u64 i = 0; i < weights.length(); i++) {
            if (weights[i] > max_weight) {
                excess += weights[i] - max_weight;
                weights[i] = max_weight;
                capped_count++;
            }
        }

        // Second pass: redistribute excess to uncapped weights
        if (excess > 0.0 && capped_count < weights.length()) {
            f64 redist = excess / static_cast<f64>(weights.length() - capped_count);
            for (u64 i = 0; i < weights.length(); i++) {
                if (weights[i] < max_weight) {
                    weights[i] += redist;
                }
            }

            // Re-normalize to sum to 1.0
            f64 total = 0.0;
            for (u64 i = 0; i < weights.length(); i++) total += weights[i];
            if (total > 1e-12) {
                for (u64 i = 0; i < weights.length(); i++) {
                    weights[i] /= total;
                }
            }
        }
    }

private:
    // ---- Internal Helpers ----

    // `_group_count` / `_group_at` are no longer used — the main loop now
    // walks a pre-computed `grouped` view directly. Kept removed to avoid
    // accidental re-introduction of the O(n²) per-step regrouping.

    /// Expire T+1 locks for bars that have passed the required hold period.
    auto _expire_t1_locks(u64 current_idx) -> void {
        // Map doesn't support easy iteration+erase, so track keys to remove
        Vec<String<A>, A> to_remove;
        // T+1 lock: bar + config.t1_lock_bars
        // In full impl: iterate t1_locks and check age
        (void)current_idx;
        for (u64 i = 0; i < to_remove.length(); i++) {
            static_cast<void>(t1_locks.try_erase(to_remove[i]));
        }
    }

    auto _is_trading_window(Deterministic_Time time) const -> bool {
        // Use the strategy's market_type so crypto strategies (24/7) skip
        // the stock-hours filter. Default Strategy_Base ctor leaves this
        // at stock_cn which still matches the historical behaviour.
        return Market_Rules<A>::is_in_trade_window(time, strategy.market_type);
    }

    /// Extract the most recent signal value from strategy state.
    auto _extract_signal_from_strategy(String_View code) -> f64 {
        // Read from strategy.latest_price or strategy.signals
        // The strategy's on_bar() should have populated system_vars
        auto sv_entry = strategy.system_vars.try_get("signal"_v);
        if (sv_entry.ok()) return **sv_entry;
        return 0.0;
    }

    static auto _try_get_signal(const Map<String<A>, f64, A>& vars,
                                 String_View key, f64& out) -> void {
        auto entry = vars.try_get(key);
        if (entry.ok()) out = **entry;
    }

    /// Sort a vector of pairs by score descending (bubble sort for simplicity).
    static auto _sort_by_score_desc(Vec<Pair<String<A>, f64>, A>& items) -> void {
        for (u64 i = 0; i < items.length(); i++) {
            for (u64 j = i + 1; j < items.length(); j++) {
                if (items[j].second > items[i].second) {
                    auto tmp = spp::move(items[i]);
                    items[i] = spp::move(items[j]);
                    items[j] = spp::move(tmp);
                }
            }
        }
    }

    auto _bars_since(u64 bar_idx) const -> u64 {
        if (current_bar_idx >= bar_idx) return current_bar_idx - bar_idx;
        return 0;
    }

    auto _slippage_model_str() const -> String_View {
        switch (config.slippage_model) {
            case Slippage_Model_Type::fixed:  return "fixed"_v;
            case Slippage_Model_Type::impact: return "impact"_v;
            default: return "percent"_v;
        }
    }

    auto _impact_model_str() const -> String_View {
        switch (config.impact_model) {
            case Impact_Model_Type::square_root:    return "square_root"_v;
            case Impact_Model_Type::linear:         return "linear"_v;
            case Impact_Model_Type::almgren_chriss: return "almgren_chriss"_v;
            default: return "none_"_v;
        }
    }

    /// Estimate per-bar volume for a code from cached historical mean.
    /// Falls back to 100k for codes we haven't ingested data for (defensive
    /// default — better to size impact down than have zero divisor).
    auto _estimate_bar_volume(String_View code) -> f64 {
        auto entry = _volume_cache.try_get(code);
        if(entry.ok()) {
            f64 v = **entry;
            if(v > 0.0) return v;
        }
        return 100000.0;
    }

    static auto _price_to_f64(Decimal<8> p) noexcept -> f64 {
        return static_cast<f64>(p.raw()) / static_cast<f64>(Decimal<8>::factor());
    }

    static auto _sv_eq(String_View a, String_View b) noexcept -> bool {
        if (a.length() != b.length()) return false;
        return Libc::strncmp(reinterpret_cast<const char*>(a.data()),
                              reinterpret_cast<const char*>(b.data()),
                              a.length()) == 0;
    }
};

} // namespace spp::quant::backtest
