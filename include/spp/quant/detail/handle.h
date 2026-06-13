#pragma once

#include <spp/core/base.h>
#include "observer.h"

namespace spp::quant {

namespace detail {

template<typename T, typename U>
void rc_move_raw(Rc<T>& dst, Rc<U>& src) noexcept {
    static_assert(sizeof(Rc<T>) == sizeof(Rc<U>));
    static_assert(alignof(Rc<T>) == alignof(Rc<U>));
    u8 tmp[sizeof(Rc<T>)];
    Libc::memcpy(tmp, &src, sizeof(Rc<T>));
    Libc::memset(&src, 0, sizeof(Rc<U>));
    Libc::memcpy(&dst, tmp, sizeof(Rc<T>));
}

} // namespace detail

template<typename T>
struct Handle {
    static_assert(Base_Of<T, Observable>, "Handle<T> requires T to derive from Observable");

    struct Link final : Observer {
        Handle* owner_ = null;
        ~Link() override = default;
        void update() override {
            if(owner_ && owner_->current_.ok()) {
                owner_->on_underlying_changed();
            }
        }
    };

    Rc<T> current_;
    Box<Link> link_;

    Handle() noexcept = default;

    template<typename U>
        requires Base_Of<U, T>
    explicit Handle(Rc<U> obj) noexcept {
        detail::rc_move_raw<T, U>(current_, obj);
        attach();
    }

    ~Handle() noexcept {
        detach();
    }

    Handle(const Handle& other) noexcept : current_(other.current_.dup()) {
        attach();
    }

    Handle& operator=(const Handle& other) noexcept {
        if(this == &other) return *this;
        detach();
        current_ = other.current_.dup();
        attach();
        return *this;
    }

    Handle(Handle&& other) noexcept {
        other.detach();
        current_ = spp::move(other.current_);
        attach();
    }

    Handle& operator=(Handle&& other) noexcept {
        if(this == &other) return *this;
        detach();
        other.detach();
        current_ = spp::move(other.current_);
        attach();
        return *this;
    }

    T* operator->() noexcept {
        assert(current_.ok());
        return &*current_;
    }
    const T* operator->() const noexcept {
        assert(current_.ok());
        return &*current_;
    }
    T& operator*() noexcept {
        assert(current_.ok());
        return *current_;
    }
    const T& operator*() const noexcept {
        assert(current_.ok());
        return *current_;
    }

    bool is_valid() const noexcept {
        return current_.ok();
    }

    bool empty() const noexcept {
        return !current_.ok();
    }

protected:
    void on_underlying_changed() noexcept {
    }

private:
    void attach() noexcept {
        if(!current_.ok()) return;
        link_ = Box<Link>::make();
        link_->owner_ = this;
        current_->register_observer(&*link_);
    }

    void detach() noexcept {
        if(link_.ok() && current_.ok()) {
            current_->unregister_observer(&*link_);
        }
    }

    friend struct Link;
    template<typename>
    friend struct Handle;
};

} // namespace spp::quant
