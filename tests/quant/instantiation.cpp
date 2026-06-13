#include "test.h"

// Force-instantiate every quant template at the public surface. Until this
// file exists, an unused template method silently rots — the bug only fires
// when a downstream user calls it. Keep this exercise here so future drift
// fails `make test` instead of someone's production attempt.

#include <spp/quant/backtest/engine.h>
#include <spp/quant/backtest/market_rules.h>
#include <spp/quant/backtest/slippage.h>
#include <spp/quant/backtest/types.h>
#include <spp/quant/broker/exchange.h>
#include <spp/quant/data/financial_data.h>
#include <spp/quant/data/level2_data.h>
#include <spp/quant/data/ohlcv_data.h>
#include <spp/quant/data/panel_data.h>
#include <spp/quant/data/resample.h>
#include <spp/quant/data/tick_data.h>
#include <spp/quant/data/types.h>
#include <spp/quant/factor/alpha101.h>
#include <spp/quant/factor/chan_fractal.h>
#include <spp/quant/factor/factor_store.h>
#include <spp/quant/factor/fft_features.h>
#include <spp/quant/factor/math_util.h>
#include <spp/quant/factor/q_transformer.h>
#include <spp/quant/factor/rolling.h>
#include <spp/quant/metrics/core_metrics.h>
#include <spp/quant/metrics/factor_metrics.h>
#include <spp/quant/risk/risk_checker.h>
#include <spp/quant/risk/risk_config.h>
#include <spp/quant/risk/stress_test.h>
#include <spp/quant/strategy/account.h>
#include <spp/quant/strategy/multi_strategy.h>
#include <spp/quant/strategy/order_relay.h>
#include <spp/quant/strategy/strategy_base.h>
#include <spp/quant/strategy/types.h>

using namespace spp::quant;

// Concrete CRTP-derived shells. Strategy_Base / Multi_Strategy need a real
// Derived type so `_derived().on_bar(...)` calls have somewhere to bind.
struct Probe_Strategy : strategy::Strategy_Base<Probe_Strategy, Mdefault> {};
struct Probe_Multi : strategy::Multi_Strategy<Probe_Multi, Mdefault> {};

// Explicit instantiations. The compiler now emits every member body and any
// latent type error (wrong Ref dereference, missing field, non-copyable type
// being copied) surfaces here.
template struct spp::quant::data::Ohlcv_Data<Mdefault>;
template struct spp::quant::data::Panel_Data<Mdefault>;
template struct spp::quant::data::Tick_Data<Mdefault>;
template struct spp::quant::data::Level2_Data<Mdefault>;
template struct spp::quant::data::Financial_Data<Mdefault>;
template struct spp::quant::data::Resampler<Mdefault>;
template struct spp::quant::factor::Chan_Features<Mdefault>;
template struct spp::quant::factor::Factor_Store<Mdefault>;
template struct spp::quant::risk::Risk_Checker<Mdefault>;
template struct spp::quant::risk::Stress_Tester<Mdefault>;
template struct spp::quant::strategy::Account<Mdefault>;
template struct spp::quant::strategy::Strategy_Base<Probe_Strategy, Mdefault>;
template struct spp::quant::strategy::Multi_Strategy<Probe_Multi, Mdefault>;
template struct spp::quant::strategy::Order_Relay<Mdefault>;
template struct spp::quant::backtest::Backtest_Engine<Probe_Strategy, Mdefault>;

i32 main() {
    Test test{"empty"_v};
    // Trivial runtime checks — the instantiation itself is the assertion.
    data::Ohlcv_Data<Mdefault> d;
    assert(d.bar_count() == 0);
    return 0;
}
