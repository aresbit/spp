#pragma once

#include <spp/concurrency/thread.h>

namespace spp::Concurrency {

template<typename T, Allocator A = Mdefault>
struct Lockfree_Ring {
private:
    struct Cell {
        Thread::Atomic sequence;
        Storage<T> storage;
    };

public:
    Lockfree_Ring() noexcept = default;

    explicit Lockfree_Ring(u64 requested_capacity) noexcept {
        init_(requested_capacity);
    }

    ~Lockfree_Ring() noexcept {
        destroy_();
    }

    Lockfree_Ring(const Lockfree_Ring&) noexcept = delete;
    Lockfree_Ring& operator=(const Lockfree_Ring&) noexcept = delete;

    Lockfree_Ring(Lockfree_Ring&& src) noexcept {
        move_from_(spp::move(src));
    }
    Lockfree_Ring& operator=(Lockfree_Ring&& src) noexcept {
        if(this == &src) return *this;
        destroy_();
        move_from_(spp::move(src));
        return *this;
    }

    struct Write_Reservation {
        Write_Reservation() noexcept = default;

        Write_Reservation(Write_Reservation&& src) noexcept
            : ring_(src.ring_), cell_(src.cell_), pos_(src.pos_), constructed_(src.constructed_),
              active_(src.active_) {
            src.ring_ = null;
            src.cell_ = null;
            src.pos_ = 0;
            src.constructed_ = false;
            src.active_ = false;
        }
        Write_Reservation& operator=(Write_Reservation&& src) noexcept {
            if(this == &src) return *this;
            cancel();
            ring_ = src.ring_;
            cell_ = src.cell_;
            pos_ = src.pos_;
            constructed_ = src.constructed_;
            active_ = src.active_;
            src.ring_ = null;
            src.cell_ = null;
            src.pos_ = 0;
            src.constructed_ = false;
            src.active_ = false;
            return *this;
        }

        Write_Reservation(const Write_Reservation&) noexcept = delete;
        Write_Reservation& operator=(const Write_Reservation&) noexcept = delete;

        ~Write_Reservation() noexcept {
            cancel();
        }

        [[nodiscard]] bool ok() const noexcept {
            return active_;
        }

        template<typename... Args>
            requires Constructable<T, Args...>
        [[nodiscard]] bool emplace(Args&&... args) noexcept {
            if(!active_ || constructed_) return false;
            cell_->storage.construct(spp::forward<Args>(args)...);
            constructed_ = true;
            return true;
        }

        [[nodiscard]] T* data() noexcept {
            if(!active_) return null;
            return reinterpret_cast<T*>(cell_->storage.data());
        }

        [[nodiscard]] const T* data() const noexcept {
            if(!active_) return null;
            return reinterpret_cast<const T*>(cell_->storage.data());
        }

        [[nodiscard]] bool commit() noexcept {
            if(!active_ || !constructed_) return false;
            ring_->commit_write_(cell_, pos_);
            active_ = false;
            return true;
        }

        void cancel() noexcept {
            if(!active_) return;
            if(constructed_) cell_->storage.destruct();
            ring_->cancel_write_(cell_, pos_);
            active_ = false;
        }

    private:
        Write_Reservation(Lockfree_Ring* ring, Cell* cell, u64 pos) noexcept
            : ring_(ring), cell_(cell), pos_(pos), active_(true) {
        }

        Lockfree_Ring* ring_ = null;
        Cell* cell_ = null;
        u64 pos_ = 0;
        bool constructed_ = false;
        bool active_ = false;

        friend struct Lockfree_Ring;
    };

    struct Read_Reservation {
        Read_Reservation() noexcept = default;

        Read_Reservation(Read_Reservation&& src) noexcept
            : ring_(src.ring_), cell_(src.cell_), pos_(src.pos_), active_(src.active_) {
            src.ring_ = null;
            src.cell_ = null;
            src.pos_ = 0;
            src.active_ = false;
        }
        Read_Reservation& operator=(Read_Reservation&& src) noexcept {
            if(this == &src) return *this;
            commit();
            ring_ = src.ring_;
            cell_ = src.cell_;
            pos_ = src.pos_;
            active_ = src.active_;
            src.ring_ = null;
            src.cell_ = null;
            src.pos_ = 0;
            src.active_ = false;
            return *this;
        }

        Read_Reservation(const Read_Reservation&) noexcept = delete;
        Read_Reservation& operator=(const Read_Reservation&) noexcept = delete;

        ~Read_Reservation() noexcept {
            commit();
        }

        [[nodiscard]] bool ok() const noexcept {
            return active_;
        }

        [[nodiscard]] T& value() noexcept {
            assert(active_);
            return *reinterpret_cast<T*>(cell_->storage.data());
        }

        [[nodiscard]] const T& value() const noexcept {
            assert(active_);
            return *reinterpret_cast<const T*>(cell_->storage.data());
        }

        void commit() noexcept {
            if(!active_) return;
            ring_->commit_read_(cell_, pos_);
            active_ = false;
        }

    private:
        Read_Reservation(Lockfree_Ring* ring, Cell* cell, u64 pos) noexcept
            : ring_(ring), cell_(cell), pos_(pos), active_(true) {
        }

        Lockfree_Ring* ring_ = null;
        Cell* cell_ = null;
        u64 pos_ = 0;
        bool active_ = false;

        friend struct Lockfree_Ring;
    };

    [[nodiscard]] bool try_push(T&& value) noexcept
        requires Move_Constructable<T>
    {
        auto reservation = try_reserve_write();
        if(!reservation.ok()) return false;
        bool ok = reservation.emplace(spp::move(value));
        assert(ok);
        return reservation.commit();
    }

    [[nodiscard]] bool try_push(const T& value) noexcept
        requires Copy_Constructable<T>
    {
        auto reservation = try_reserve_write();
        if(!reservation.ok()) return false;
        bool ok = reservation.emplace(value);
        assert(ok);
        return reservation.commit();
    }

    [[nodiscard]] Opt<T> try_pop() noexcept
        requires Move_Constructable<T>
    {
        auto reservation = try_reserve_read();
        if(!reservation.ok()) return {};
        T out = spp::move(reservation.value());
        reservation.commit();
        return Opt<T>{spp::move(out)};
    }

    [[nodiscard]] bool try_pop_into(T& out) noexcept {
        auto reservation = try_reserve_read();
        if(!reservation.ok()) return false;
        out = spp::move(reservation.value());
        reservation.commit();
        return true;
    }

    [[nodiscard]] Write_Reservation try_reserve_write() noexcept {
        if(!cells_) return {};

        u64 pos = static_cast<u64>(enqueue_pos_.load());
        for(;;) {
            Cell* cell = &cells_[pos & mask_];
            i64 seq = cell->sequence.load();
            i64 dif = seq - static_cast<i64>(pos);
            if(dif == 0) {
                if(cas_u64_(enqueue_pos_, pos, pos + 1)) {
                    return Write_Reservation{this, cell, pos};
                }
            } else if(dif < 0) {
                return {};
            } else {
                pos = static_cast<u64>(enqueue_pos_.load());
            }
            Thread::pause();
        }
    }

    [[nodiscard]] Read_Reservation try_reserve_read() noexcept {
        if(!cells_) return {};

        u64 pos = static_cast<u64>(dequeue_pos_.load());
        for(;;) {
            Cell* cell = &cells_[pos & mask_];
            i64 seq = cell->sequence.load();
            i64 dif = seq - static_cast<i64>(pos + 1);
            if(dif == 0) {
                if(cas_u64_(dequeue_pos_, pos, pos + 1)) {
                    return Read_Reservation{this, cell, pos};
                }
            } else if(dif < 0) {
                return {};
            } else {
                pos = static_cast<u64>(dequeue_pos_.load());
            }
            Thread::pause();
        }
    }

    [[nodiscard]] u64 capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return cells_ != null && capacity_ > 0;
    }

    [[nodiscard]] u64 approx_size() const noexcept {
        u64 enq = static_cast<u64>(enqueue_pos_.load());
        u64 deq = static_cast<u64>(dequeue_pos_.load());
        return enq >= deq ? enq - deq : 0;
    }

private:
    static_assert(alignof(Cell) >= alignof(T));

    [[nodiscard]] static bool cas_u64_(Thread::Atomic& atom, u64 expected, u64 desired) noexcept {
        return static_cast<u64>(atom.compare_and_swap(static_cast<i64>(expected),
                                                      static_cast<i64>(desired))) == expected;
    }

    [[nodiscard]] static u64 ceil_pow2_(u64 x) noexcept {
        if(x <= 1) return 1;
        x--;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        x |= x >> 32;
        return x + 1;
    }

    void init_(u64 requested_capacity) noexcept {
        capacity_ = ceil_pow2_(requested_capacity ? requested_capacity : 1);
        mask_ = capacity_ - 1;
        cells_ = reinterpret_cast<Cell*>(A::alloc(sizeof(Cell) * capacity_));
        assert(cells_);

        for(u64 i = 0; i < capacity_; i++) {
            new(&cells_[i]) Cell{};
            static_cast<void>(cells_[i].sequence.exchange(static_cast<i64>(i)));
        }
        static_cast<void>(enqueue_pos_.exchange(0));
        static_cast<void>(dequeue_pos_.exchange(0));
    }

    void move_from_(Lockfree_Ring&& src) noexcept {
        cells_ = src.cells_;
        capacity_ = src.capacity_;
        mask_ = src.mask_;
        static_cast<void>(enqueue_pos_.exchange(src.enqueue_pos_.load()));
        static_cast<void>(dequeue_pos_.exchange(src.dequeue_pos_.load()));

        src.cells_ = null;
        src.capacity_ = 0;
        src.mask_ = 0;
        static_cast<void>(src.enqueue_pos_.exchange(0));
        static_cast<void>(src.dequeue_pos_.exchange(0));
    }

    void destroy_() noexcept {
        if(!cells_) return;
        auto reservation = try_reserve_read();
        while(reservation.ok()) {
            reservation.commit();
            reservation = try_reserve_read();
        }
        for(u64 i = 0; i < capacity_; i++) {
            cells_[i].~Cell();
        }
        A::free(cells_);
        cells_ = null;
        capacity_ = 0;
        mask_ = 0;
    }

    void commit_write_(Cell* cell, u64 pos) noexcept {
        static_cast<void>(cell->sequence.exchange(static_cast<i64>(pos + 1)));
    }

    void cancel_write_(Cell* cell, u64 pos) noexcept {
        static_cast<void>(cell->sequence.exchange(static_cast<i64>(pos)));
    }

    void commit_read_(Cell* cell, u64 pos) noexcept {
        cell->storage.destruct();
        static_cast<void>(cell->sequence.exchange(static_cast<i64>(pos + mask_ + 1)));
    }

    Cell* cells_ = null;
    u64 capacity_ = 0;
    u64 mask_ = 0;
    Thread::Atomic enqueue_pos_{0};
    Thread::Atomic dequeue_pos_{0};
};

} // namespace spp::Concurrency
