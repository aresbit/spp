#pragma once

#ifndef SPP_BASE
#error "Include base.h instead."
#endif

namespace spp {

template<typename T>
struct Ok {
    explicit Ok(T&& value) noexcept : value_(spp::move(value)) {
    }
    T value_;
};

template<typename T>
Ok(T) -> Ok<T>;

template<typename E>
struct Err {
    explicit Err(E&& value) noexcept : value_(spp::move(value)) {
    }
    E value_;
};

template<typename E>
Err(E) -> Err<E>;

template<typename T, typename E>
struct Result {

    Result() noexcept = delete;

    explicit Result(Ok<T>&& ok) noexcept
        requires Move_Constructable<T>
        : ok_(true) {
        value_.construct(spp::move(ok.value_));
    }

    explicit Result(Err<E>&& err) noexcept
        requires Move_Constructable<E>
        : ok_(false) {
        error_.construct(spp::move(err.value_));
    }

    ~Result() noexcept {
        clear();
    }

    Result(const Result& src) noexcept
        requires Copy_Constructable<T> && Copy_Constructable<E>
        : ok_(src.ok_) {
        if(ok_) {
            value_.construct(*src.value_);
        } else {
            error_.construct(*src.error_);
        }
    }

    Result& operator=(const Result& src) noexcept
        requires Copy_Constructable<T> && Copy_Constructable<E>
    {
        if(this == &src) return *this;
        clear();
        ok_ = src.ok_;
        if(ok_) {
            value_.construct(*src.value_);
        } else {
            error_.construct(*src.error_);
        }
        return *this;
    }

    Result(Result&& src) noexcept
        requires Move_Constructable<T> && Move_Constructable<E>
        : ok_(src.ok_) {
        if(ok_) {
            value_.construct(spp::move(*src.value_));
        } else {
            error_.construct(spp::move(*src.error_));
        }
    }

    Result& operator=(Result&& src) noexcept
        requires Move_Constructable<T> && Move_Constructable<E>
    {
        if(this == &src) return *this;
        clear();
        ok_ = src.ok_;
        if(ok_) {
            value_.construct(spp::move(*src.value_));
        } else {
            error_.construct(spp::move(*src.error_));
        }
        return *this;
    }

    [[nodiscard]] static Result ok(T&& value) noexcept
        requires Move_Constructable<T>
    {
        return Result{Ok<T>{spp::move(value)}};
    }

    [[nodiscard]] static Result err(E&& value) noexcept
        requires Move_Constructable<E>
    {
        return Result{Err<E>{spp::move(value)}};
    }

    template<typename... Args>
        requires Constructable<T, Args...>
    void emplace_ok(Args&&... args) noexcept {
        clear();
        ok_ = true;
        value_.construct(spp::forward<Args>(args)...);
    }

    template<typename... Args>
        requires Constructable<E, Args...>
    void emplace_err(Args&&... args) noexcept {
        clear();
        ok_ = false;
        error_.construct(spp::forward<Args>(args)...);
    }

    void clear() noexcept {
        if(ok_) {
            if constexpr(Must_Destruct<T>) value_.destruct();
        } else {
            if constexpr(Must_Destruct<E>) error_.destruct();
        }
    }

    [[nodiscard]] Result clone() const noexcept
        requires(Clone<T> || Copy_Constructable<T>) && (Clone<E> || Copy_Constructable<E>)
    {
        if(ok_) {
            if constexpr(Clone<T>) {
                return Result{Ok<T>{value_->clone()}};
            } else {
                static_assert(Copy_Constructable<T>);
                return Result{Ok<T>{T{*value_}}};
            }
        }

        if constexpr(Clone<E>) {
            return Result{Err<E>{error_->clone()}};
        } else {
            static_assert(Copy_Constructable<E>);
            return Result{Err<E>{E{*error_}}};
        }
    }

    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }

    [[nodiscard]] T& unwrap() noexcept {
        assert(ok_);
        return *value_;
    }
    [[nodiscard]] const T& unwrap() const noexcept {
        assert(ok_);
        return *value_;
    }

    [[nodiscard]] E& unwrap_err() noexcept {
        assert(!ok_);
        return *error_;
    }
    [[nodiscard]] const E& unwrap_err() const noexcept {
        assert(!ok_);
        return *error_;
    }

private:
    bool ok_ = false;
    Storage<T> value_;
    Storage<E> error_;

    friend struct Reflect::Refl<Result>;
};

template<typename T, typename E>
SPP_TEMPLATE_RECORD(Result, SPP_PACK(T, E), SPP_FIELD(ok_), SPP_FIELD(value_), SPP_FIELD(error_));

namespace Format {

template<Reflectable T, Reflectable E>
struct Measure<Result<T, E>> {
    [[nodiscard]] static u64 measure(const Result<T, E>& result) noexcept {
        if(result.ok()) return 11 + Measure<T>::measure(result.unwrap());
        return 12 + Measure<E>::measure(result.unwrap_err());
    }
};

template<Allocator O, Reflectable T, Reflectable E>
struct Write<O, Result<T, E>> {
    [[nodiscard]] static u64 write(String<O>& output, u64 idx, const Result<T, E>& result) noexcept {
        if(result.ok()) {
            idx = output.write(idx, "Result{Ok("_v);
            idx = Write<O, T>::write(output, idx, result.unwrap());
            return output.write(idx, ")}"_v);
        }
        idx = output.write(idx, "Result{Err("_v);
        idx = Write<O, E>::write(output, idx, result.unwrap_err());
        return output.write(idx, ")}"_v);
    }
};

} // namespace Format

} // namespace spp
