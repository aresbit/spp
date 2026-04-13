#pragma once

#include <spp/concurrency/thread.h>

namespace spp::Concurrency {

template<Key K, Move_Constructable V, Allocator A = Thread::Alloc>
struct Concurrent_Map {
    Concurrent_Map() noexcept = default;
    explicit Concurrent_Map(u64 capacity) noexcept : map_(capacity) {
    }

    void clear() noexcept {
        Thread::Lock lock{mutex_};
        map_.clear();
    }

    [[nodiscard]] u64 length() const noexcept {
        Thread::Lock lock{mutex_};
        return map_.length();
    }

    [[nodiscard]] bool contains(const K& key) const noexcept {
        Thread::Lock lock{mutex_};
        return map_.contains(key);
    }

    V& insert(K&& key, V&& value) noexcept {
        Thread::Lock lock{mutex_};
        return map_.insert(spp::move(key), spp::move(value));
    }

    V& insert(const K& key, const V& value) noexcept
        requires Copy_Constructable<K> && Copy_Constructable<V>
    {
        Thread::Lock lock{mutex_};
        return map_.insert(key, value);
    }

    [[nodiscard]] bool erase(const K& key) noexcept {
        Thread::Lock lock{mutex_};
        return map_.try_erase(key);
    }

    template<typename F>
    V& get_or_insert_with(const K& key, F&& make_value) noexcept
        requires Copy_Constructable<K> && Invocable<F> && Constructable<V, Invoke_Result<F>>
    {
        Thread::Lock lock{mutex_};
        auto found = map_.try_get(key);
        if(found.ok()) return **found;
        return map_.insert(K{key}, V{spp::forward<F>(make_value)()});
    }

    template<typename F>
    V& get_or_insert_with(K&& key, F&& make_value) noexcept
        requires Invocable<F> && Constructable<V, Invoke_Result<F>>
    {
        Thread::Lock lock{mutex_};
        auto found = map_.try_get(key);
        if(found.ok()) return **found;
        return map_.insert(spp::move(key), V{spp::forward<F>(make_value)()});
    }

    template<typename FI, typename FU>
    V& upsert(const K& key, FI&& on_insert, FU&& on_update) noexcept
        requires Copy_Constructable<K> && Invocable<FI> && Constructable<V, Invoke_Result<FI>> &&
                 Invocable<FU, V&>
    {
        Thread::Lock lock{mutex_};
        auto found = map_.try_get(key);
        if(found.ok()) {
            spp::forward<FU>(on_update)(**found);
            return **found;
        }
        return map_.insert(K{key}, V{spp::forward<FI>(on_insert)()});
    }

    template<typename FI, typename FU>
    V& upsert(K&& key, FI&& on_insert, FU&& on_update) noexcept
        requires Invocable<FI> && Constructable<V, Invoke_Result<FI>> && Invocable<FU, V&>
    {
        Thread::Lock lock{mutex_};
        auto found = map_.try_get(key);
        if(found.ok()) {
            spp::forward<FU>(on_update)(**found);
            return **found;
        }
        return map_.insert(spp::move(key), V{spp::forward<FI>(on_insert)()});
    }

    template<typename F>
    [[nodiscard]] bool update_if(const K& key, F&& updater) noexcept
        requires Invocable<F, V&>
    {
        Thread::Lock lock{mutex_};
        auto found = map_.try_get(key);
        if(!found.ok()) return false;
        spp::forward<F>(updater)(**found);
        return true;
    }

    template<typename F>
    [[nodiscard]] bool erase_if(const K& key, F&& predicate) noexcept
        requires Invocable<F, const V&> && Same<Invoke_Result<F, const V&>, bool>
    {
        Thread::Lock lock{mutex_};
        auto found = map_.try_get(key);
        if(!found.ok()) return false;
        if(!spp::forward<F>(predicate)(**found)) return false;
        return map_.try_erase(key);
    }

    [[nodiscard]] Opt<V> try_get_copy(const K& key) const noexcept
        requires Clone<V> || Copy_Constructable<V>
    {
        Thread::Lock lock{mutex_};
        auto value = map_.try_get(key);
        if(!value.ok()) return {};
        if constexpr(Clone<V>) {
            return Opt<V>{(**value).clone()};
        } else {
            static_assert(Copy_Constructable<V>);
            return Opt<V>{V{**value}};
        }
    }

    template<typename F>
    [[nodiscard]] auto read(F&& f) const noexcept -> Invoke_Result<F, const Map<K, V, A>&> {
        Thread::Lock lock{mutex_};
        if constexpr(Same<Invoke_Result<F, const Map<K, V, A>&>, void>) {
            spp::forward<F>(f)(map_);
            return;
        } else {
            return spp::forward<F>(f)(map_);
        }
    }

    template<typename F>
    [[nodiscard]] auto write(F&& f) noexcept -> Invoke_Result<F, Map<K, V, A>&> {
        Thread::Lock lock{mutex_};
        if constexpr(Same<Invoke_Result<F, Map<K, V, A>&>, void>) {
            spp::forward<F>(f)(map_);
            return;
        } else {
            return spp::forward<F>(f)(map_);
        }
    }

    template<typename F>
    [[nodiscard]] auto with_lock(F&& f) noexcept -> Invoke_Result<F, Map<K, V, A>&> {
        return write(spp::forward<F>(f));
    }

    template<typename F>
    [[nodiscard]] auto with_lock(F&& f) const noexcept -> Invoke_Result<F, const Map<K, V, A>&> {
        return read(spp::forward<F>(f));
    }

private:
    mutable Thread::Mutex mutex_;
    Map<K, V, A> map_;
};

template<typename T, Allocator A = Thread::Alloc>
struct Concurrent_Vec {
    Concurrent_Vec() noexcept = default;
    explicit Concurrent_Vec(u64 capacity) noexcept : vec_(capacity) {
    }

    [[nodiscard]] u64 length() const noexcept {
        Thread::Lock lock{mutex_};
        return vec_.length();
    }

    void clear() noexcept {
        Thread::Lock lock{mutex_};
        vec_.clear();
    }

    T& push(T&& value) noexcept
        requires Move_Constructable<T>
    {
        Thread::Lock lock{mutex_};
        return vec_.push(spp::move(value));
    }

    T& push(const T& value) noexcept
        requires Copy_Constructable<T>
    {
        Thread::Lock lock{mutex_};
        return vec_.push(value);
    }

    [[nodiscard]] Opt<T> try_pop() noexcept
        requires Move_Constructable<T>
    {
        Thread::Lock lock{mutex_};
        if(vec_.empty()) return {};
        T out = spp::move(vec_.back());
        vec_.pop();
        return Opt<T>{spp::move(out)};
    }

    template<typename F>
    [[nodiscard]] auto read(F&& f) const noexcept -> Invoke_Result<F, const Vec<T, A>&> {
        Thread::Lock lock{mutex_};
        if constexpr(Same<Invoke_Result<F, const Vec<T, A>&>, void>) {
            spp::forward<F>(f)(vec_);
            return;
        } else {
            return spp::forward<F>(f)(vec_);
        }
    }

    template<typename F>
    [[nodiscard]] auto write(F&& f) noexcept -> Invoke_Result<F, Vec<T, A>&> {
        Thread::Lock lock{mutex_};
        if constexpr(Same<Invoke_Result<F, Vec<T, A>&>, void>) {
            spp::forward<F>(f)(vec_);
            return;
        } else {
            return spp::forward<F>(f)(vec_);
        }
    }

    template<typename F>
    [[nodiscard]] auto with_lock(F&& f) noexcept -> Invoke_Result<F, Vec<T, A>&> {
        return write(spp::forward<F>(f));
    }

    template<typename F>
    [[nodiscard]] auto with_lock(F&& f) const noexcept -> Invoke_Result<F, const Vec<T, A>&> {
        return read(spp::forward<F>(f));
    }

private:
    mutable Thread::Mutex mutex_;
    Vec<T, A> vec_;
};

} // namespace spp::Concurrency
