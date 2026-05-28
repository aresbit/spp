#pragma once

#include <spp/core/base.h>
#include <spp/quant/instruments/instrument.h>
#include <spp/quant/base/currency.h>

namespace spp::quant {

// =========================================================================
// Stock — single equity position
// =========================================================================
struct Stock : Instrument {
    String_View   ticker_;
    Currency_Code currency_ = Currency_Code::USD;
    f64           price_    = 0.0;

    f64  npv()       const override { return price_; }
    bool is_expired() const override { return false; }

    SPP_RECORD(Stock,
        SPP_FIELD(ticker_),
        SPP_FIELD(currency_),
        SPP_FIELD(price_));
};

// =========================================================================
// EquityIndex — broad market or sector index
// =========================================================================
struct EquityIndex : Instrument {
    String_View name_;
    f64         level_ = 0.0;

    f64  npv()       const override { return level_; }
    bool is_expired() const override { return false; }

    SPP_RECORD(EquityIndex,
        SPP_FIELD(name_),
        SPP_FIELD(level_));
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::Stock, "Stock",
    SPP_FIELD(ticker_), SPP_FIELD(currency_), SPP_FIELD(price_));

SPP_NAMED_RECORD(::spp::quant::EquityIndex, "EquityIndex",
    SPP_FIELD(name_), SPP_FIELD(level_));
