#pragma once

#include <spp/core/base.h>

namespace spp::quant {

enum struct Currency_Code : u16 {
    USD = 840,
    EUR = 978,
    GBP = 826,
    JPY = 392,
    CHF = 756,
    AUD = 36,
    CAD = 124,
    CNY = 156,
    HKD = 344,
    KRW = 410,
    INR = 356,
    SGD = 702,
    TWD = 901,
    BRL = 986,
    MXN = 484,
    RUB = 643,
    ZAR = 710,
    TRY = 949,
    SEK = 752,
    NOK = 578,
    DKK = 208,
    NZD = 554,
    THB = 764,
    MYR = 458,
    IDR = 360,
    PHP = 608,
    PLN = 985,
    CZK = 203,
    HUF = 348,
    ILS = 376,
    CLP = 152,
    COP = 170,
    PEN = 604,
    SAR = 682,
    AED = 784,
    XAU = 959,
    XAG = 961,
};

struct Currency {
    Currency_Code code_ = Currency_Code::USD;
    String_View name_ = "US Dollar"_v;
    String_View symbol_ = "$"_v;
    i32 numeric_code_ = 840;
    u8 minor_unit_digits_ = 2;

    bool operator==(const Currency& other) const noexcept {
        return code_ == other.code_;
    }
    bool operator!=(const Currency& other) const noexcept {
        return code_ != other.code_;
    }

    static Currency usd() noexcept {
        return {Currency_Code::USD, "US Dollar"_v, "$"_v, 840, 2};
    }
    static Currency eur() noexcept {
        return {Currency_Code::EUR, "Euro"_v, "\xE2\x82\xAc"_v, 978, 2};
    }
    static Currency gbp() noexcept {
        return {Currency_Code::GBP, "Pound Sterling"_v, "\xC2\xA3"_v, 826, 2};
    }
    static Currency jpy() noexcept {
        return {Currency_Code::JPY, "Japanese Yen"_v, "\xC2\xA5"_v, 392, 0};
    }
    static Currency chf() noexcept {
        return {Currency_Code::CHF, "Swiss Franc"_v, "CHF"_v, 756, 2};
    }
    static Currency aud() noexcept {
        return {Currency_Code::AUD, "Australian Dollar"_v, "A$"_v, 36, 2};
    }
    static Currency cad() noexcept {
        return {Currency_Code::CAD, "Canadian Dollar"_v, "C$"_v, 124, 2};
    }
    static Currency cny() noexcept {
        return {Currency_Code::CNY, "Chinese Yuan"_v, "\xC2\xA5"_v, 156, 2};
    }
    static Currency hkd() noexcept {
        return {Currency_Code::HKD, "Hong Kong Dollar"_v, "HK$"_v, 344, 2};
    }
    static Currency krw() noexcept {
        return {Currency_Code::KRW, "South Korean Won"_v, "\xE2\x82\xA9"_v, 410, 0};
    }
    static Currency inr() noexcept {
        return {Currency_Code::INR, "Indian Rupee"_v, "\xE2\x82\xB9"_v, 356, 2};
    }
    static Currency sgd() noexcept {
        return {Currency_Code::SGD, "Singapore Dollar"_v, "S$"_v, 702, 2};
    }
    static Currency twd() noexcept {
        return {Currency_Code::TWD, "Taiwan Dollar"_v, "NT$"_v, 901, 2};
    }
    static Currency brl() noexcept {
        return {Currency_Code::BRL, "Brazilian Real"_v, "R$"_v, 986, 2};
    }
    static Currency mxn() noexcept {
        return {Currency_Code::MXN, "Mexican Peso"_v, "Mex$"_v, 484, 2};
    }
    static Currency rub() noexcept {
        return {Currency_Code::RUB, "Russian Ruble"_v, "\xE2\x82\xBD"_v, 643, 2};
    }
    static Currency zar() noexcept {
        return {Currency_Code::ZAR, "South African Rand"_v, "R"_v, 710, 2};
    }
    static Currency try_() noexcept {
        return {Currency_Code::TRY, "Turkish Lira"_v, "\xE2\x82\xBA"_v, 949, 2};
    }
    static Currency sek() noexcept {
        return {Currency_Code::SEK, "Swedish Krona"_v, "kr"_v, 752, 2};
    }
    static Currency nok() noexcept {
        return {Currency_Code::NOK, "Norwegian Krone"_v, "kr"_v, 578, 2};
    }
    static Currency dkk() noexcept {
        return {Currency_Code::DKK, "Danish Krone"_v, "kr"_v, 208, 2};
    }
    static Currency nzd() noexcept {
        return {Currency_Code::NZD, "New Zealand Dollar"_v, "NZ$"_v, 554, 2};
    }
    static Currency thb() noexcept {
        return {Currency_Code::THB, "Thai Baht"_v, "\xE0\xB8\xBF"_v, 764, 2};
    }
};

struct Money {
    f64 amount_ = 0.0;
    Currency_Code currency_ = Currency_Code::USD;

    Money() = default;
    Money(f64 amount, Currency c) noexcept : amount_(amount), currency_(c.code_) {
    }
    Money(f64 amount, Currency_Code cc) noexcept : amount_(amount), currency_(cc) {
    }

    Money operator+(const Money& other) const noexcept {
        assert(currency_ == other.currency_);
        return Money{amount_ + other.amount_, currency_};
    }
    Money operator-(const Money& other) const noexcept {
        assert(currency_ == other.currency_);
        return Money{amount_ - other.amount_, currency_};
    }
    Money operator-() const noexcept {
        return Money{-amount_, currency_};
    }
    Money operator*(f64 scalar) const noexcept {
        return Money{amount_ * scalar, currency_};
    }
    Money operator/(f64 scalar) const noexcept {
        return Money{amount_ / scalar, currency_};
    }

    Money& operator+=(const Money& other) noexcept {
        assert(currency_ == other.currency_);
        amount_ += other.amount_;
        return *this;
    }
    Money& operator-=(const Money& other) noexcept {
        assert(currency_ == other.currency_);
        amount_ -= other.amount_;
        return *this;
    }
    Money& operator*=(f64 scalar) noexcept {
        amount_ *= scalar;
        return *this;
    }
    Money& operator/=(f64 scalar) noexcept {
        amount_ /= scalar;
        return *this;
    }

    bool operator==(const Money& other) const noexcept {
        return currency_ == other.currency_ && amount_ == other.amount_;
    }
    bool operator!=(const Money& other) const noexcept {
        return !(*this == other);
    }
    bool operator<(const Money& other) const noexcept {
        assert(currency_ == other.currency_);
        return amount_ < other.amount_;
    }
    bool operator<=(const Money& other) const noexcept {
        assert(currency_ == other.currency_);
        return amount_ <= other.amount_;
    }
    bool operator>(const Money& other) const noexcept {
        assert(currency_ == other.currency_);
        return amount_ > other.amount_;
    }
    bool operator>=(const Money& other) const noexcept {
        assert(currency_ == other.currency_);
        return amount_ >= other.amount_;
    }

    f64 amount() const noexcept {
        return amount_;
    }
    Currency_Code currency() const noexcept {
        return currency_;
    }
};

inline Money operator*(f64 scalar, const Money& m) noexcept {
    return m * scalar;
}

} // namespace spp::quant

SPP_NAMED_ENUM(::spp::quant::Currency_Code, "Currency_Code", USD, SPP_CASE(USD), SPP_CASE(EUR),
               SPP_CASE(GBP), SPP_CASE(JPY), SPP_CASE(CHF), SPP_CASE(AUD), SPP_CASE(CAD),
               SPP_CASE(CNY), SPP_CASE(HKD), SPP_CASE(KRW), SPP_CASE(INR), SPP_CASE(SGD),
               SPP_CASE(TWD), SPP_CASE(BRL), SPP_CASE(MXN), SPP_CASE(RUB), SPP_CASE(ZAR),
               SPP_CASE(TRY), SPP_CASE(SEK), SPP_CASE(NOK), SPP_CASE(DKK), SPP_CASE(NZD),
               SPP_CASE(THB), SPP_CASE(MYR), SPP_CASE(IDR), SPP_CASE(PHP), SPP_CASE(PLN),
               SPP_CASE(CZK), SPP_CASE(HUF), SPP_CASE(ILS), SPP_CASE(CLP), SPP_CASE(COP),
               SPP_CASE(PEN), SPP_CASE(SAR), SPP_CASE(AED), SPP_CASE(XAU), SPP_CASE(XAG));

SPP_NAMED_RECORD(::spp::quant::Currency, "Currency", SPP_FIELD(code_), SPP_FIELD(name_),
                 SPP_FIELD(symbol_), SPP_FIELD(numeric_code_), SPP_FIELD(minor_unit_digits_));

SPP_NAMED_RECORD(::spp::quant::Money, "Money", SPP_FIELD(amount_), SPP_FIELD(currency_));
