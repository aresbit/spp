#pragma once

#include <spp/core/base.h>
#include <spp/quant/instruments/instrument.h>
#include <spp/quant/base/date.h>

namespace spp::quant {

// =========================================================================
// EuropeanOption — exercise only at expiry
// =========================================================================
struct EuropeanOption : Instrument {
    OptionType type_            = OptionType::Call;
    f64        strike_          = 100.0;
    Date       expiry_;
    f64        underlying_spot_ = 100.0;

    f64 npv() const override {
        // placeholder — real pricing via engine dispatcher
        return 0.0;
    }

    bool is_expired() const override {
        return Date::today() >= expiry_;
    }

    f64 payoff(f64 spot) const noexcept {
        if (type_ == OptionType::Call)
            return Math::max(spot - strike_, 0.0);
        else
            return Math::max(strike_ - spot, 0.0);
    }

    SPP_RECORD(EuropeanOption,
        SPP_FIELD(type_),
        SPP_FIELD(strike_),
        SPP_FIELD(expiry_),
        SPP_FIELD(underlying_spot_));
};

// =========================================================================
// AmericanOption — early exercise permitted any time before expiry
// =========================================================================
struct AmericanOption : Instrument {
    OptionType type_            = OptionType::Call;
    f64        strike_          = 100.0;
    Date       expiry_;
    f64        underlying_spot_ = 100.0;

    f64 npv() const override { return 0.0; }

    bool is_expired() const override {
        return Date::today() >= expiry_;
    }

    f64 payoff(f64 spot) const noexcept {
        if (type_ == OptionType::Call)
            return Math::max(spot - strike_, 0.0);
        else
            return Math::max(strike_ - spot, 0.0);
    }

    SPP_RECORD(AmericanOption,
        SPP_FIELD(type_),
        SPP_FIELD(strike_),
        SPP_FIELD(expiry_),
        SPP_FIELD(underlying_spot_));
};

// =========================================================================
// BarrierOption — knock-in / knock-out with continuous monitoring
// =========================================================================
struct BarrierOption : Instrument {
    enum struct BarrierType : u8 {
        UpAndOut   = 0,
        UpAndIn    = 1,
        DownAndOut = 2,
        DownAndIn  = 3,
    };

    OptionType  type_            = OptionType::Call;
    f64         strike_          = 100.0;
    f64         barrier_         = 120.0;
    BarrierType barrier_type_    = BarrierType::UpAndOut;
    Date        expiry_;
    f64         underlying_spot_ = 100.0;

    f64 npv() const override { return 0.0; }

    bool is_expired() const override {
        return Date::today() >= expiry_;
    }

    SPP_RECORD(BarrierOption,
        SPP_FIELD(type_),
        SPP_FIELD(strike_),
        SPP_FIELD(barrier_),
        SPP_FIELD(barrier_type_),
        SPP_FIELD(expiry_),
        SPP_FIELD(underlying_spot_));
};

// =========================================================================
// AsianOption — payoff based on average price over the option's life
// =========================================================================
struct AsianOption : Instrument {
    enum struct AverageType : u8 {
        Arithmetic = 0,
        Geometric  = 1,
    };

    OptionType  type_            = OptionType::Call;
    f64         strike_          = 100.0;
    Date        expiry_;
    AverageType avg_type_        = AverageType::Arithmetic;
    f64         underlying_spot_ = 100.0;

    f64 npv() const override { return 0.0; }

    bool is_expired() const override {
        return Date::today() >= expiry_;
    }

    SPP_RECORD(AsianOption,
        SPP_FIELD(type_),
        SPP_FIELD(strike_),
        SPP_FIELD(expiry_),
        SPP_FIELD(avg_type_),
        SPP_FIELD(underlying_spot_));
};

// =========================================================================
// DigitalOption — cash-or-nothing binary option
// =========================================================================
struct DigitalOption : Instrument {
    OptionType type_            = OptionType::Call;
    f64        strike_          = 100.0;
    f64        cash_rebate_     = 1.0;
    Date       expiry_;
    f64        underlying_spot_ = 100.0;

    f64 npv() const override { return 0.0; }

    bool is_expired() const override {
        return Date::today() >= expiry_;
    }

    SPP_RECORD(DigitalOption,
        SPP_FIELD(type_),
        SPP_FIELD(strike_),
        SPP_FIELD(cash_rebate_),
        SPP_FIELD(expiry_),
        SPP_FIELD(underlying_spot_));
};

// =========================================================================
// Variant-based tagged union for dispatch
// =========================================================================
using AnyOption = Variant<EuropeanOption,
                           AmericanOption,
                           BarrierOption,
                           AsianOption,
                           DigitalOption>;

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::EuropeanOption, "EuropeanOption",
    SPP_FIELD(type_), SPP_FIELD(strike_), SPP_FIELD(expiry_),
    SPP_FIELD(underlying_spot_));

SPP_NAMED_RECORD(::spp::quant::AmericanOption, "AmericanOption",
    SPP_FIELD(type_), SPP_FIELD(strike_), SPP_FIELD(expiry_),
    SPP_FIELD(underlying_spot_));

SPP_NAMED_ENUM(::spp::quant::BarrierOption::BarrierType, "BarrierType", UpAndOut,
    SPP_CASE(UpAndOut), SPP_CASE(UpAndIn),
    SPP_CASE(DownAndOut), SPP_CASE(DownAndIn));

SPP_NAMED_RECORD(::spp::quant::BarrierOption, "BarrierOption",
    SPP_FIELD(type_), SPP_FIELD(strike_), SPP_FIELD(barrier_),
    SPP_FIELD(barrier_type_), SPP_FIELD(expiry_), SPP_FIELD(underlying_spot_));

SPP_NAMED_ENUM(::spp::quant::AsianOption::AverageType, "AverageType", Arithmetic,
    SPP_CASE(Arithmetic), SPP_CASE(Geometric));

SPP_NAMED_RECORD(::spp::quant::AsianOption, "AsianOption",
    SPP_FIELD(type_), SPP_FIELD(strike_), SPP_FIELD(expiry_),
    SPP_FIELD(avg_type_), SPP_FIELD(underlying_spot_));

SPP_NAMED_RECORD(::spp::quant::DigitalOption, "DigitalOption",
    SPP_FIELD(type_), SPP_FIELD(strike_), SPP_FIELD(cash_rebate_),
    SPP_FIELD(expiry_), SPP_FIELD(underlying_spot_));
