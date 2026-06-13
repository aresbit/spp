#pragma once

#include <spp/core/base.h>
#include <spp/quant/detail/observer.h>
#include <spp/quant/detail/handle.h>

namespace spp::quant {

struct Quote : Observable {
    Quote() = default;
    ~Quote() override = default;

    virtual f64 value() const = 0;
};

struct SimpleQuote final : Quote {
    f64 value_ = 0.0;

    SimpleQuote() = default;

    explicit SimpleQuote(f64 v) noexcept : value_(v) {
    }

    f64 value() const noexcept override {
        return value_;
    }

    void set_value(f64 v) noexcept {
        value_ = v;
        notify_observers();
    }
};

struct DerivedQuote final : Quote {
    struct Watcher final : Observer {
        DerivedQuote* owner_ = null;
        ~Watcher() override = default;
        void update() override {
            if(owner_) owner_->notify_observers();
        }
    };

    Handle<Quote> underlying_;
    f64 (*transform_)(f64) = null;
    Box<Watcher> watcher_;

    DerivedQuote() = default;

    explicit DerivedQuote(Handle<Quote> underlying, f64 (*tf)(f64) = null) noexcept
        : underlying_(spp::move(underlying)), transform_(tf) {
        attach();
    }

    ~DerivedQuote() noexcept override {
        detach();
    }

    DerivedQuote(const DerivedQuote& other) noexcept
        : underlying_(other.underlying_), transform_(other.transform_) {
        attach();
    }

    DerivedQuote& operator=(const DerivedQuote& other) noexcept {
        if(this == &other) return *this;
        detach();
        underlying_ = other.underlying_;
        transform_ = other.transform_;
        attach();
        return *this;
    }

    DerivedQuote(DerivedQuote&& other) noexcept
        : underlying_(spp::move(other.underlying_)), transform_(other.transform_) {
        attach();
    }

    DerivedQuote& operator=(DerivedQuote&& other) noexcept {
        if(this == &other) return *this;
        detach();
        underlying_ = spp::move(other.underlying_);
        transform_ = other.transform_;
        attach();
        return *this;
    }

    f64 value() const noexcept override {
        f64 v = underlying_->value();
        return transform_ ? transform_(v) : v;
    }

private:
    void attach() noexcept {
        if(!underlying_.is_valid()) return;
        watcher_ = Box<Watcher>::make();
        watcher_->owner_ = this;
        underlying_->register_observer(&*watcher_);
    }

    void detach() noexcept {
        if(watcher_.ok() && underlying_.is_valid()) {
            underlying_->unregister_observer(&*watcher_);
        }
    }

    friend struct Watcher;
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::Quote, "Quote");
