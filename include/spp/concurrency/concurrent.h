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

private:
    mutable Thread::Mutex mutex_;
    Vec<T, A> vec_;
};

} // namespace spp::Concurrency

