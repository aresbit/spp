#pragma once

#include <spp/core/base.h>

namespace spp::quant {

// =========================================================================
// Greeks — sensitivity bundle returned by all pricing engines
// =========================================================================
struct Greeks {
    f64 delta = 0.0;
    f64 gamma = 0.0;
    f64 theta = 0.0;
    f64 vega  = 0.0;
    f64 rho   = 0.0;
};

// =========================================================================
// Option exercise style
// =========================================================================
enum struct ExerciseType : u8 {
    European = 0,
    American = 1,
    Bermudan = 2,
};

// =========================================================================
// Call / Put
// =========================================================================
enum struct OptionType : u8 {
    Call = 0,
    Put  = 1,
};

// =========================================================================
// Instrument — pure abstract base for all financial instruments
// =========================================================================
struct Instrument {
    virtual f64  npv()       const = 0;
    virtual bool is_expired() const = 0;
    virtual ~Instrument()          = default;
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::Greeks, "Greeks",
    SPP_FIELD(delta), SPP_FIELD(gamma), SPP_FIELD(theta),
    SPP_FIELD(vega),  SPP_FIELD(rho));

SPP_NAMED_ENUM(::spp::quant::ExerciseType, "ExerciseType", European,
    SPP_CASE(European), SPP_CASE(American), SPP_CASE(Bermudan));

SPP_NAMED_ENUM(::spp::quant::OptionType, "OptionType", Call,
    SPP_CASE(Call), SPP_CASE(Put));
