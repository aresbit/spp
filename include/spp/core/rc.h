
#pragma once

#include <spp/core/base.h>

namespace spp {

namespace detail {

template<typename T>
struct Rc_Data {
    explicit Rc_Data(u64 r) noexcept : references(r) {
    }

    u64 references;
    Storage<T> value;
};

template<typename T>
struct Arc_Data {
    explicit Arc_Data(Thread::Atomic r) noexcept : references(r) {
    }

    Thread::Atomic references;
    Storage<T> value;
};

} // namespace detail

template<typename T, Scalar_Allocator P>
struct Rc;

template<typename T, Scalar_Allocator P = Mdefault>
Rc(T) -> Rc<T, P>;

template<typename T, Scalar_Allocator P = Mdefault>
struct Rc {
    using A = Pool_Adaptor<P>;
    using Data = detail::Rc_Data<T>;

    Rc() noexcept = default;
    ~Rc() noexcept {
        drop();
    }

    template<typename... Args>
        requires Constructable<T, Args...>
    explicit Rc(Args&&... args) noexcept {
        data_ = A::template make<Data>(static_cast<u64>(1));
        data_->value.construct(spp::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] static Rc make(Args&&... args) noexcept {
        Rc ret;
        ret.data_ = A::template make<Data>(static_cast<u64>(1));
        new(ret.data_->value.data()) T{spp::forward<Args>(args)...};
        return ret;
    }

    static Rc from_this(T* value) noexcept {
        Rc ret;
        ret.data_ =
            reinterpret_cast<Data*>(reinterpret_cast<u8*>(value) - SPP_OFFSETOF(Data, value));
        ret.data_->references++;
        return ret;
    }

    Rc(const Rc& src) noexcept = delete;
    Rc& operator=(const Rc& src) noexcept = delete;

    [[nodiscard]] Rc dup() const noexcept {
        Rc ret;
        ret.data_ = data_;
        if(data_) data_->references++;
        return ret;
    }

    template<Scalar_Allocator R = P>
    [[nodiscard]] Rc<T, R> clone() const noexcept
        requires(Clone<T> || Copy_Constructable<T>)
    {
        if(!data_) return Rc<T, R>{};
        if constexpr(Clone<T>) {
            return Rc<T, R>{(*data_->value).clone()};
        } else {
            static_assert(Copy_Constructable<T>);
            return Rc<T, R>{*data_->value};
        }
    }

    Rc(Rc&& src) noexcept {
        data_ = src.data_;
        src.data_ = null;
    }
    Rc& operator=(Rc&& src) noexcept {
        drop();
        data_ = src.data_;
        src.data_ = null;
        return *this;
    }

    [[nodiscard]] T* operator->() noexcept {
        assert(data_);
        return &*data_->value;
    }
    [[nodiscard]] const T* operator->() const noexcept {
        assert(data_);
        return &*data_->value;
    }
    [[nodiscard]] T& operator*() noexcept {
        assert(data_);
        return *data_->value;
    }
    [[nodiscard]] const T& operator*() const noexcept {
        assert(data_);
        return *data_->value;
    }

    [[nodiscard]] u64 references() const noexcept {
        return data_ ? data_->references : 0;
    }
    [[nodiscard]] bool ok() const noexcept {
        return data_ != null;
    }
    void clear() noexcept {
        drop();
    }

private:
    void drop() noexcept {
        if(!data_) return;
        data_->references--;
        if(data_->references == 0) {
            data_->value.destruct();
            A::template destroy<Data>(data_);
        }
        data_ = null;
    }

    Data* data_ = null;

    friend struct Reflect::Refl<Rc<T>>;
};

template<typename T, Scalar_Allocator P>
struct Arc;

template<typename T, Scalar_Allocator P = Mdefault>
Arc(T) -> Arc<T, P>;

template<typename T, Scalar_Allocator P = Mdefault>
struct Arc {
    using A = Pool_Adaptor<P>;
    using Data = detail::Arc_Data<T>;

    Arc() noexcept = default;
    ~Arc() noexcept {
        drop();
    }

    template<typename... Args>
        requires Constructable<T, Args...>
    explicit Arc(Args&&... args) noexcept {
        data_ = A::template make<Data>(Thread::Atomic{1});
        data_->value.construct(spp::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] static Arc make(Args&&... args) noexcept {
        Arc ret;
        ret.data_ = A::template make<Data>(Thread::Atomic{1});
        new(ret.data_->value.data()) T{spp::forward<Args>(args)...};
        return ret;
    }

    static Arc from_this(T* value) noexcept {
        Arc ret;
        ret.data_ =
            reinterpret_cast<Data*>(reinterpret_cast<u8*>(value) - SPP_OFFSETOF(Data, value));
        ret.data_->references.incr();
        return ret;
    }

    Arc(const Arc& src) noexcept = delete;
    Arc& operator=(const Arc& src) noexcept = delete;

    [[nodiscard]] Arc dup() const noexcept {
        Arc ret;
        ret.data_ = data_;
        if(data_) data_->references.incr();
        return ret;
    }

    template<Scalar_Allocator R = P>
    [[nodiscard]] Arc<T, R> clone() const noexcept
        requires(Clone<T> || Copy_Constructable<T>)
    {
        if(!data_) return Arc<T, R>{};
        if constexpr(Clone<T>) {
            return Arc<T, R>{(*data_->value).clone()};
        } else {
            static_assert(Copy_Constructable<T>);
            return Arc<T, R>{*data_->value};
        }
    }

    Arc(Arc&& src) noexcept {
        data_ = src.data_;
        src.data_ = null;
    }
    Arc& operator=(Arc&& src) noexcept {
        drop();
        data_ = src.data_;
        src.data_ = null;
        return *this;
    }

    [[nodiscard]] T* operator->() noexcept {
        assert(data_);
        return &*data_->value;
    }
    [[nodiscard]] const T* operator->() const noexcept {
        assert(data_);
        return &*data_->value;
    }
    [[nodiscard]] T& operator*() noexcept {
        assert(data_);
        return *data_->value;
    }
    [[nodiscard]] const T& operator*() const noexcept {
        assert(data_);
        return *data_->value;
    }

    [[nodiscard]] u64 references() const noexcept {
        return data_ ? data_->references.load() : 0;
    }
    [[nodiscard]] bool ok() const noexcept {
        return data_ != null;
    }
    void clear() {
        drop();
    }

private:
    void drop() noexcept {
        if(!data_) return;
        if(data_->references.decr() == 0) {
            data_->value.destruct();
            A::template destroy<Data>(data_);
        }
        data_ = null;
    }

    Data* data_ = null;

    friend struct Reflect::Refl<Arc<T>>;
};

template<typename T>
SPP_NAMED_TEMPLATE_RECORD(::spp::detail::Rc_Data, "Rc_Data", T, SPP_FIELD(references),
                          SPP_FIELD(value));

template<typename T>
SPP_NAMED_TEMPLATE_RECORD(::spp::detail::Arc_Data, "Arc_Data", T, SPP_FIELD(references),
                          SPP_FIELD(value));

template<typename T>
SPP_TEMPLATE_RECORD(Rc, T, SPP_FIELD(data_));

template<typename T>
SPP_TEMPLATE_RECORD(Arc, T, SPP_FIELD(data_));

namespace Format {

template<Reflectable T, Allocator A>
struct Measure<Rc<T, A>> {
    [[nodiscard]] static u64 measure(const Rc<T, A>& rc) noexcept {
        if(rc.ok())
            return 6 + Measure<T>::measure(*rc) +
                   Measure<decltype(rc.references())>::measure(rc.references());
        return 8;
    }
};
template<Reflectable T, Allocator A>
struct Measure<Arc<T, A>> {
    [[nodiscard]] static u64 measure(const Arc<T, A>& arc) noexcept {
        if(arc.ok())
            return 7 + Measure<T>::measure(*arc) +
                   Measure<decltype(arc.references())>::measure(arc.references());
        return 9;
    }
};

template<Allocator O, Reflectable T, Allocator A>
struct Write<O, Rc<T, A>> {
    [[nodiscard]] static u64 write(String<O>& output, u64 idx, const Rc<T, A>& rc) noexcept {
        if(!rc.ok()) return output.write(idx, "Rc{null}"_v);
        idx = output.write(idx, "Rc["_v);
        idx = Write<O, decltype(rc.references())>::write(output, idx, rc.references());
        idx = output.write(idx, "]{"_v);
        idx = Write<O, T>::write(output, idx, *rc);
        return output.write(idx, '}');
    }
};
template<Allocator O, Reflectable T, Allocator A>
struct Write<O, Arc<T, A>> {
    [[nodiscard]] static u64 write(String<O>& output, u64 idx, const Arc<T, A>& arc) noexcept {
        if(!arc.ok()) return output.write(idx, "Arc{null}"_v);
        idx = output.write(idx, "Arc["_v);
        idx = Write<O, decltype(arc.references())>::write(output, idx, arc.references());
        idx = output.write(idx, "]{"_v);
        idx = Write<O, T>::write(output, idx, *arc);
        return output.write(idx, '}');
    }
};

} // namespace Format

} // namespace spp
