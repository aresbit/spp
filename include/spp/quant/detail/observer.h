#pragma once

#include <spp/core/base.h>

namespace spp::quant {

struct Observable;

struct Observer {
    Observer() = default;
    virtual ~Observer() = default;
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    Observer(Observer&&) = delete;
    Observer& operator=(Observer&&) = delete;

    virtual void update() = 0;
};

struct Observable {
    Vec<Observer*> observers_;

    Observable() = default;
    virtual ~Observable() = default;
    Observable(const Observable&) = delete;
    Observable& operator=(const Observable&) = delete;
    Observable(Observable&&) = delete;
    Observable& operator=(Observable&&) = delete;

    void notify_observers() noexcept {
        for(u64 i = 0; i < observers_.length(); i++) {
            observers_[i]->update();
        }
    }

    void register_observer(Observer* obs) noexcept {
        if(!obs) return;
        for(u64 i = 0; i < observers_.length(); i++) {
            if(observers_[i] == obs) return;
        }
        observers_.push(obs);
    }

    void unregister_observer(Observer* obs) noexcept {
        for(u64 i = 0; i < observers_.length(); i++) {
            if(observers_[i] == obs) {
                observers_[i] = observers_.back();
                observers_.pop();
                return;
            }
        }
    }
};

} // namespace spp::quant

SPP_NAMED_RECORD(::spp::quant::Observer, "Observer");
