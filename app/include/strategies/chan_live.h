#pragma once

// Chan (缠论) fractal-driven 1-minute strategy.
//
// Trading rule (deliberately minimal — this is a reference, not the
// final alpha you'd put real money on):
//
//   When a NEW *bottom fractal* is detected → go long at bar close
//   When a NEW *top fractal* is detected    → close long at bar close
//
// "New" means the fractal didn't exist in the previous bar's analysis;
// we de-duplicate by the fractal's bar index.
//
// Position sizing: a fraction of available cash, configurable. Stop loss
// optional and tied to the most recent OPPOSITE fractal price.
//
// The strategy delegates fractal/stroke/pivot maintenance to
// `Chan_Live_Features` (streaming) so each bar costs O(window) of work
// regardless of how long the strategy has been running.

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/numeric/math.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/data/types.h>
#include <spp/quant/factor/chan_fractal.h>
#include <spp/quant/factor/chan_live.h>
#include <spp/quant/strategy/strategy_base.h>
#include <spp/quant/strategy/types.h>

namespace spp::App::Strategies {

struct Chan_Live_Config {
    // Symbol the strategy trades. The driver's WS subscription must
    // include this in lower-case + "@aggTrade".
    String_View symbol = "BTCUSDT"_v;

    // Detection window for `detect_fractals` (the ± look-around). A
    // 1-min chart with window=5 means each fractal candidate is checked
    // against 10 neighbours.
    u32 fractal_window = 5;

    // Reserved. The incremental detector keeps only `2*fractal_window+1`
    // bars in memory; this field is kept for ABI compatibility with the
    // pre-incremental Chan_Live_Strategy but is no longer consulted.
    u64 max_history_bars = 240;

    // Position sizing as a fraction of `acc.available` to commit on
    // entry. 0.95 leaves a tiny cushion for fees.
    f64 entry_cash_fraction = 0.95;

    // If non-zero, a stop loss is placed `stop_buffer_pct` away from
    // the entry price (in the unfavourable direction). 0 disables.
    f64 stop_buffer_pct = 0.0;
};

struct Chan_Live_Strategy
    : spp::quant::strategy::Strategy_Base<Chan_Live_Strategy, Mdefault> {

    Chan_Live_Config cfg;

    // Streaming detector — replaces the old "keep a rolling
    // Ohlcv_Data + re-run detect_fractals every bar" pattern with
    // O(window) work per bar regardless of session length.
    spp::quant::factor::Chan_Live_Features<Mdefault> chan;

    // Bar index of the most recent fractal we ACTED on. Fractals carry
    // an absolute bar index (via Chan_Live_Features.bars_pushed), so no
    // window-relative translation is needed.
    Opt<u64> last_acted_top_;
    Opt<u64> last_acted_bottom_;

    // Stop reference: price of the opposite fractal at entry, used for
    // exit triggers in `on_bar`.
    Opt<spp::Decimal<8>> stop_price_long_;
    Opt<spp::Decimal<8>> stop_price_short_;

    // Diagnostic counters.
    u64 fractals_seen_top = 0;
    u64 fractals_seen_bottom = 0;
    u64 entries_long = 0;
    u64 exits_long = 0;
    u64 stop_hits = 0;

    Chan_Live_Strategy() noexcept = default;
    explicit Chan_Live_Strategy(Chan_Live_Config c) noexcept
        : cfg(spp::move(c)),
          chan(cfg.fractal_window) {
        market_type = spp::quant::strategy::Market_Type::crypto;
        codes.push(cfg.symbol.template string<Mdefault>());
    }

    // Strategy_Base::x1 calls this with the closed bar. The incremental
    // detector handles the rolling-window state internally; we just push
    // the bar and read off the current latest-of-each-type fractal.
    void on_bar(const spp::quant::data::Bar& bar) noexcept {
        chan.on_bar(bar);

        // 1. Stop-loss check first — a fractal that turns against us
        //    while still in-position should liquidate at this bar's
        //    close, no waiting for the next opposite fractal.
        if(stop_price_long_.ok()) {
            if(bar.close <= *stop_price_long_) {
                close_long_(bar);
                stop_hits++;
                return;
            }
        }

        // 2. The detector keeps fractals strictly alternating after the
        //    merge pass, so the most recent top and bottom are at the
        //    tail (length-1 and length-2 if we have at least two).
        const auto& fractals = chan.fractals;
        if(fractals.length() == 0) return;

        using FT = spp::quant::factor::Fractal_Type;
        Opt<u64> latest_top, latest_bottom;
        u64 last = fractals.length() - 1;
        if(fractals[last].type == FT::top) {
            latest_top = Opt<u64>{fractals[last].index};
            if(fractals.length() >= 2 && fractals[last - 1].type == FT::bottom) {
                latest_bottom = Opt<u64>{fractals[last - 1].index};
            }
        } else {
            latest_bottom = Opt<u64>{fractals[last].index};
            if(fractals.length() >= 2 && fractals[last - 1].type == FT::top) {
                latest_top = Opt<u64>{fractals[last - 1].index};
            }
        }

        if(latest_bottom.ok()) {
            u64 abs_idx = *latest_bottom;
            if(!last_acted_bottom_.ok() || *last_acted_bottom_ != abs_idx) {
                fractals_seen_bottom++;
                last_acted_bottom_ = Opt<u64>{abs_idx};
                maybe_enter_long_(bar);
            }
        }
        if(latest_top.ok()) {
            u64 abs_idx = *latest_top;
            if(!last_acted_top_.ok() || *last_acted_top_ != abs_idx) {
                fractals_seen_top++;
                last_acted_top_ = Opt<u64>{abs_idx};
                maybe_exit_long_(bar);
            }
        }
    }

private:
    void maybe_enter_long_(const spp::quant::data::Bar& bar) noexcept {
        // Skip if already long.
        auto pos = acc.get_position(cfg.symbol);
        if(pos.ok() && pos->volume_long > 0.0) return;

        f64 cash = acc.available * cfg.entry_cash_fraction;
        if(cash <= 0.0) return;

        f64 px = spp::quant::data::price_to_f64(bar.close);
        if(px <= 0.0) return;

        f64 qty = cash / px;
        if(qty <= 0.0) return;

        auto r = send_order(spp::quant::strategy::Order_Direction::buy,
                            spp::quant::strategy::Order_Offset::open_,
                            bar.close, qty, cfg.symbol);
        if(r.ok()) {
            entries_long++;
            if(cfg.stop_buffer_pct > 0.0) {
                f64 stop_px = px * (1.0 - cfg.stop_buffer_pct);
                stop_price_long_ = Opt<spp::Decimal<8>>{
                    spp::quant::data::f64_to_price(stop_px)};
            }
        }
    }

    void maybe_exit_long_(const spp::quant::data::Bar& bar) noexcept {
        auto pos = acc.get_position(cfg.symbol);
        if(!pos.ok() || pos->volume_long <= 0.0) return;
        close_long_(bar);
    }

    void close_long_(const spp::quant::data::Bar& bar) noexcept {
        auto pos = acc.get_position(cfg.symbol);
        if(!pos.ok() || pos->volume_long <= 0.0) return;

        auto r = send_order(spp::quant::strategy::Order_Direction::sell,
                            spp::quant::strategy::Order_Offset::close_,
                            bar.close, pos->volume_long, cfg.symbol);
        if(r.ok()) {
            exits_long++;
            stop_price_long_ = Opt<spp::Decimal<8>>{};
        }
    }
};

} // namespace spp::App::Strategies
