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
    using Ok_Type = T;
    using Err_Type = E;

    Result() noexcept
        requires Default_Constructable<E>
        : ok_(false) {
        error_.construct(E{});
    }

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

    template<typename F>
        requires Invocable<F, T&&> && Move_Constructable<E>
    [[nodiscard]] auto map(F&& f) && noexcept -> Result<Decay<Invoke_Result<F, T&&>>, E> {
        using U = Decay<Invoke_Result<F, T&&>>;
        if(ok_) {
            return Result<U, E>::ok(spp::forward<F>(f)(spp::move(*value_)));
        }
        return Result<U, E>::err(spp::move(*error_));
    }

    template<typename F>
        requires Invocable<F, const T&> && Copy_Constructable<E>
    [[nodiscard]] auto map(F&& f) const& noexcept -> Result<Decay<Invoke_Result<F, const T&>>, E> {
        using U = Decay<Invoke_Result<F, const T&>>;
        if(ok_) {
            return Result<U, E>::ok(spp::forward<F>(f)(*value_));
        }
        return Result<U, E>::err(E{*error_});
    }

    template<typename F>
        requires Invocable<F, E&&> && Move_Constructable<T>
    [[nodiscard]] auto map_err(F&& f) && noexcept -> Result<T, Decay<Invoke_Result<F, E&&>>> {
        using E2 = Decay<Invoke_Result<F, E&&>>;
        if(ok_) {
            return Result<T, E2>::ok(spp::move(*value_));
        }
        return Result<T, E2>::err(spp::forward<F>(f)(spp::move(*error_)));
    }

    template<typename F>
        requires Invocable<F, const E&> && Copy_Constructable<T>
    [[nodiscard]] auto map_err(F&& f) const& noexcept
        -> Result<T, Decay<Invoke_Result<F, const E&>>> {
        using E2 = Decay<Invoke_Result<F, const E&>>;
        if(ok_) {
            return Result<T, E2>::ok(T{*value_});
        }
        return Result<T, E2>::err(spp::forward<F>(f)(*error_));
    }

    template<typename F>
        requires Invocable<F, T&&> && Same<typename Decay<Invoke_Result<F, T&&>>::Err_Type, E> &&
                 Move_Constructable<E>
    [[nodiscard]] auto and_then(F&& f) && noexcept -> Decay<Invoke_Result<F, T&&>> {
        using R = Decay<Invoke_Result<F, T&&>>;
        if(ok_) {
            return spp::forward<F>(f)(spp::move(*value_));
        }
        return R::err(spp::move(*error_));
    }

    template<typename F>
        requires Invocable<F, const T&> &&
                 Same<typename Decay<Invoke_Result<F, const T&>>::Err_Type, E> &&
                 Copy_Constructable<E>
    [[nodiscard]] auto and_then(F&& f) const& noexcept -> Decay<Invoke_Result<F, const T&>> {
        using R = Decay<Invoke_Result<F, const T&>>;
        if(ok_) {
            return spp::forward<F>(f)(*value_);
        }
        return R::err(E{*error_});
    }

    template<typename F>
        requires Invocable<F, E&&> && Same<typename Decay<Invoke_Result<F, E&&>>::Ok_Type, T> &&
                 Move_Constructable<T>
    [[nodiscard]] auto or_else(F&& f) && noexcept -> Decay<Invoke_Result<F, E&&>> {
        using R = Decay<Invoke_Result<F, E&&>>;
        if(ok_) {
            return R::ok(spp::move(*value_));
        }
        return spp::forward<F>(f)(spp::move(*error_));
    }

    template<typename F>
        requires Invocable<F, const E&> &&
                 Same<typename Decay<Invoke_Result<F, const E&>>::Ok_Type, T> &&
                 Copy_Constructable<T>
    [[nodiscard]] auto or_else(F&& f) const& noexcept -> Decay<Invoke_Result<F, const E&>> {
        using R = Decay<Invoke_Result<F, const E&>>;
        if(ok_) {
            return R::ok(T{*value_});
        }
        return spp::forward<F>(f)(*error_);
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
