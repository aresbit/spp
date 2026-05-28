#pragma once

// =========================================================================
// Master include header for the spp-quant library.
// Include this to pull in all quant modules.
// =========================================================================

// ---- Base ----------------------------------------------------------------
#include "spp/quant/base/date.h"
#include "spp/quant/base/calendar.h"
#include "spp/quant/base/daycounter.h"
#include "spp/quant/base/currency.h"
#include "spp/quant/base/quote.h"
#include "spp/quant/base/schedule.h"

// ---- Detail (observer / handle infrastructure) ---------------------------
#include "spp/quant/detail/observer.h"
#include "spp/quant/detail/handle.h"

// ---- Math -----------------------------------------------------------------
#include "spp/quant/math/distributions.h"
#include "spp/quant/math/statistics.h"
#include "spp/quant/math/interpolation.h"
#include "spp/quant/math/solvers.h"
#include "spp/quant/math/random.h"
#include "spp/quant/math/matrix.h"

// ---- Data -----------------------------------------------------------------
#include "spp/quant/data/timeseries.h"
#include "spp/quant/data/market_data.h"

// ---- Risk ----------------------------------------------------------------
#include "spp/quant/risk/greeks.h"
#include "spp/quant/risk/var.h"
#include "spp/quant/risk/stress.h"
#include "spp/quant/risk/sensitivity.h"

// ---- Portfolio -----------------------------------------------------------
#include "spp/quant/portfolio/position.h"
#include "spp/quant/portfolio/portfolio.h"
#include "spp/quant/portfolio/optimization.h"

// ---- Performance ---------------------------------------------------------
#include "spp/quant/perf/returns.h"
#include "spp/quant/perf/ratios.h"
#include "spp/quant/perf/drawdown.h"

// ---- Backtest ------------------------------------------------------------
#include "spp/quant/backtest/event.h"
#include "spp/quant/backtest/cost_model.h"
#include "spp/quant/backtest/broker.h"
#include "spp/quant/backtest/engine.h"
#include "spp/quant/backtest/metrics.h"

// ---- Instruments --------------------------------------------------------
#include "spp/quant/instruments/instrument.h"
#include "spp/quant/instruments/equity.h"
#include "spp/quant/instruments/fx.h"
#include "spp/quant/instruments/options.h"
#include "spp/quant/instruments/bonds.h"

// ---- Pricing -------------------------------------------------------------
#include "spp/quant/pricing/engine.h"
#include "spp/quant/pricing/black_scholes.h"
#include "spp/quant/pricing/binomial.h"

// ---- Term Structures -----------------------------------------------------
#include "spp/quant/termstructures/termstructure.h"
#include "spp/quant/termstructures/yield_curve.h"
#include "spp/quant/termstructures/vol_surface.h"

// ---- Strategy ------------------------------------------------------------
#include "spp/quant/strategy/strategy.h"
#include "spp/quant/strategy/signal.h"
#include "spp/quant/strategy/stat_arb.h"

// ---- Calibration ---------------------------------------------------------
#include "spp/quant/calib/calibrator.h"
#include "spp/quant/calib/yield_fit.h"
#include "spp/quant/calib/optimizers.h"

// ---- Execution -----------------------------------------------------------
#include "spp/quant/execution/order.h"
#include "spp/quant/execution/algo.h"
