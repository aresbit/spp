#pragma once

#include <spp/core/base.h>
#include <spp/quant/instruments/instrument.h>
#include <spp/quant/base/currency.h>

namespace spp::quant {

// =========================================================================
// FxSpot — single currency pair spot rate
// rate_ is units of domestic currency per 1 unit of foreign currency.
// e.g. USD/CNY = 7.25 means 7.25 CNY (domestic) per 1 USD (foreign).
// =========================================================================
struct FxSpot : Instrument {
    Currency_Code domestic_ = Currency_Code::USD;
    Currency_Code foreign_  = Currency_Code::USD;
    f64           rate_     = 1.0;

    f64  npv()       const override { return rate_; }
    bool is_expired() const override { return false; }

    SPP_RECORD(FxSpot,
        SPP_FIELD(domestic_),
        SPP_FIELD(foreign_),
        SPP_FIELD(rate_));
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::FxSpot, "FxSpot",
    SPP_FIELD(domestic_), SPP_FIELD(foreign_), SPP_FIELD(rate_));
