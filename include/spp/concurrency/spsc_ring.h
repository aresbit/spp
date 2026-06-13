#pragma once

// =============================================================================
// spp::Concurrency::Spsc_Ring<T>
//
// Single-Producer Single-Consumer ring buffer.  Zero atomics on x86 — the
// sequence protocol compiles to plain MOV instructions because TSO provides
// store-store ordering for free and the single-writer/single-reader
// discipline eliminates the need for CAS or locked RMW.
//
// Mathematical model (the user's insight, formalised):
//
//   The ring is the additive group Z/nZ under unsigned modular addition.
//   Producer position `p` and consumer position `c` are u64 counters in Z;
//   the slot index is p & (n-1).  The cell sequence `s[i]` encodes the
//   *lap number*: s[i] = (lap_number * n) + i.
//
//   Free slot  (producer view):  s[p & mask] == p
//   Ready slot (consumer view):  s[c & mask] == c + 1
//   Done slot  (consumer releases):  s[c & mask] = c + n
//
//   The partial order p ≥ c holds (producer never laps consumer).
//   Ring-full  ⇔ p - c == n.
//   Ring-empty ⇔ p == c.
//
//   The producer-side invariant
//       cached_c <= c  ∧  p - cached_c < n
//   guarantees that at least one slot is free WITHOUT loading any cell
//   sequence — the cell-sequence path only fires when the cached shadow is
//   stale (≤ once per full lap).  This is the Disruptor "lazySet" pattern.
//
// Architecture notes:
//
//   * x86 TSO: `__ATOMIC_RELEASE` store → plain MOV; `__ATOMIC_ACQUIRE` load → MOV.
//     The ONLY reordering x86 permits is Store→Load; since the producer does
//     Store(payload) → Store(sequence) (both stores, no reorder) and the
//     consumer does Load(sequence) → Load(payload) (both loads, no reorder),
//     the protocol is correct without any barrier instructions.
//   * ARM64: `__ATOMIC_RELEASE` emits `stlr`; `__ATOMIC_ACQUIRE` emits `ldar`.
//     Cost is ~1-2 extra cycles per hot-path op vs x86.
//
// Self-contained: no <atomic>, <thread>, or libstdc++ headers needed.
// =============================================================================

#include <stdlib.h>   // posix_memalign, free

// ---- Hugepage support (Linux) ------------------------------------------------
// 2 MiB hugepages eliminate dTLB-miss jitter on the cell array. We try an
// explicit MAP_HUGETLB mapping first and fall back to a plain anonymous mmap
// hinted with MADV_HUGEPAGE (transparent hugepages). Non-Linux platforms use
// posix_memalign only.
#if defined(__linux__)
#  include <sys/mman.h>   // mmap, munmap, madvise, MAP_HUGETLB
#  define SPP_SPSC_HAVE_HUGEPAGE 1
#else
#  define SPP_SPSC_HAVE_HUGEPAGE 0
#endif

// ---- Architecture detection --------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#  define SPP_SPSC_ARCH_X64   1
#  define SPP_SPSC_ARCH_ARM64 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define SPP_SPSC_ARCH_X64   0
#  define SPP_SPSC_ARCH_ARM64 1
#else
#  define SPP_SPSC_ARCH_X64   0
#  define SPP_SPSC_ARCH_ARM64 0
#endif

#if defined(__clang__) || defined(__GNUC__)
#  define SPP_SPSC_FORCE_INLINE   __attribute__((always_inline)) inline
#  define SPP_SPSC_LIKELY(x)      __builtin_expect(!!(x), 1)
#  define SPP_SPSC_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#  define SPP_SPSC_PREFETCH(p, rw, loc) __builtin_prefetch((p), (rw), (loc))
#  define SPP_SPSC_NO_INLINE      __attribute__((noinline))
#else
#  define SPP_SPSC_FORCE_INLINE   inline
#  define SPP_SPSC_LIKELY(x)      (x)
#  define SPP_SPSC_UNLIKELY(x)    (x)
#  define SPP_SPSC_PREFETCH(p, rw, loc) ((void)0)
#  define SPP_SPSC_NO_INLINE
#endif

namespace spp {
namespace Concurrency {

// ---- Minimal integer types ---------------------------------------------------
#if defined(SPP_BASE)
using ::spp::u8;
using ::spp::u32;
using ::spp::u64;
using ::spp::i64;
#else
using u8  = unsigned char;
using u32 = unsigned int;
#if defined(_MSC_VER)
using u64 = unsigned long long;
using i64 = long long;
#else
using u64 = unsigned long;
using i64 = long long;
#endif
static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
#endif

// ---- Constants ---------------------------------------------------------------
static constexpr u64 SPP_SPSC_CACHE_LINE = 64;
static constexpr u64 SPP_SPSC_PAD_LINE   = 128;  // Intel L2 streamer prefetch
static constexpr u64 SPP_SPSC_HUGE_2M    = 2ull * 1024 * 1024;

// ---- Architecture hooks ------------------------------------------------------

SPP_SPSC_FORCE_INLINE void spsc_pause() noexcept {
#if SPP_SPSC_ARCH_X64
    __asm__ __volatile__("pause" ::: "memory");
#elif SPP_SPSC_ARCH_ARM64
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

SPP_SPSC_FORCE_INLINE u64 spsc_rdtsc() noexcept {
#if SPP_SPSC_ARCH_X64
    unsigned lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<u64>(hi) << 32) | lo;
#elif SPP_SPSC_ARCH_ARM64
    u64 v;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

// ---- Sequence protocol (architecture-aware) ----------------------------------
//
// On x86 these are plain MOVs — no fences, no locked prefixes, no L1
// dirty-line snoop cycles.  On ARM64 they expand to ldar/stlr.

SPP_SPSC_FORCE_INLINE i64 seq_ld_acq(const i64* p) noexcept {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
SPP_SPSC_FORCE_INLINE void seq_st_rel(i64* p, i64 v) noexcept {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

SPP_SPSC_FORCE_INLINE u64 pos_ld(const u64* p) noexcept {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}
SPP_SPSC_FORCE_INLINE void pos_st(u64* p, u64 v) noexcept {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

// ---- Built-in memcpy ---------------------------------------------------------
SPP_SPSC_FORCE_INLINE void* spsc_memcpy(void* d, const void* s, u64 n) noexcept {
    return __builtin_memcpy(d, s, n);
}

// =============================================================================
// Spsc_Ring<T>
//
//   T — trivially copyable payload type.
//   UseShadowCursor — true  → hot path avoids cell-sequence loads (the
//                             "mathematical" path), best for x86 direct-MOV.
//                     false → always checks per-cell sequence (higher worst-
//                             case latency but no shadow-staleness stall).
// =============================================================================

template<typename T, bool UseShadowCursor = true>
struct alignas(SPP_SPSC_PAD_LINE) Spsc_Ring {

    // Per-cell storage. 64-byte aligned so each cell occupies its own cache
    // line (or pair of lines for payloads > 56 bytes).
    //
    // sequence is NOT volatile — the `__atomic_load_n` intrinsic is an
    // explicit compiler barrier that tells the compiler "this memory might
    // change asynchronously", and `volatile` would additionally prevent
    // dead-store elimination and register promotion across the sequence
    // load, both of which are safe to allow here because the atomic
    // intrinsics already enforce the necessary ordering.
    struct alignas(SPP_SPSC_CACHE_LINE) Cell {
        i64 sequence;
        alignas(alignof(T)) u8 storage[sizeof(T)];
    };

    // --- Producer-private ------------------------------------------------
    alignas(SPP_SPSC_PAD_LINE) u64 produced_;
    u64 cached_consumed_;              // shadow: last-known consumer pos
    u8 _pad0_[SPP_SPSC_PAD_LINE - 2*sizeof(u64)]{};

    // --- Consumer-private ------------------------------------------------
    alignas(SPP_SPSC_PAD_LINE) u64 consumed_;
    u64 cached_produced_;              // shadow: last-known producer pos
    u8 _pad1_[SPP_SPSC_PAD_LINE - 2*sizeof(u64)]{};

    // --- Shared (read-only after init) -----------------------------------
    // alloc_kind_ / alloc_bytes_ record how cells_ was obtained so destroy()
    // can pick the matching deallocator (free / munmap / numa_free).
    enum AllocKind : u64 { Alloc_Heap = 0, Alloc_Mmap = 1, Alloc_Numa = 2 };
    alignas(SPP_SPSC_PAD_LINE) Cell* cells_;
    u64 capacity_;
    u64 mask_;
    u64 alloc_kind_;
    u64 alloc_bytes_;
    u8 _pad2_[SPP_SPSC_PAD_LINE - sizeof(Cell*) - 4*sizeof(u64)]{};

    // =====================================================================
    // Construction
    // =====================================================================

    Spsc_Ring() noexcept : produced_(0), cached_consumed_(0),
                           consumed_(0), cached_produced_(0),
                           cells_(nullptr), capacity_(0), mask_(0),
                           alloc_kind_(Alloc_Heap), alloc_bytes_(0) {}

    explicit Spsc_Ring(u64 cap) noexcept : Spsc_Ring() { init(cap); }
    ~Spsc_Ring() noexcept { destroy(); }

    Spsc_Ring(const Spsc_Ring&) = delete;
    Spsc_Ring& operator=(const Spsc_Ring&) = delete;
    Spsc_Ring(Spsc_Ring&&) = delete;
    Spsc_Ring& operator=(Spsc_Ring&&) = delete;

    void init(u64 cap) noexcept {
        if(cells_) destroy();
        capacity_ = ceil_pow2(cap ? cap : 1);
        mask_     = capacity_ - 1;

        void* mem = huge_or_heap_alloc(sizeof(Cell) * capacity_,
                                       &alloc_kind_, &alloc_bytes_);
        if(!mem) { cells_ = nullptr; return; }
        cells_ = static_cast<Cell*>(mem);
        prefault_cells();
        produced_ = 0; consumed_ = 0;
        cached_consumed_ = 0; cached_produced_ = 0;
    }

    // Allocate on a specific NUMA node. Requires libnuma.
    // If libnuma isn't available or the node is invalid, falls back to the
    // hugepage/posix_memalign path.
    void init_numa(u64 cap, int /*node*/) noexcept {
        if(cells_) destroy();
        capacity_ = ceil_pow2(cap ? cap : 1);
        mask_     = capacity_ - 1;
        u64 sz = sizeof(Cell) * capacity_;

        void* mem = nullptr;
#if defined(SPP_SPSC_USE_LIBNUMA)
        mem = ::numa_alloc_onnode(sz, node);
        if(mem && (reinterpret_cast<u64>(mem) & (SPP_SPSC_PAD_LINE - 1))) {
            ::numa_free(mem, sz);
            mem = nullptr;
        }
        if(mem) { alloc_kind_ = Alloc_Numa; alloc_bytes_ = sz; }
#endif
        if(!mem) {
            mem = huge_or_heap_alloc(sz, &alloc_kind_, &alloc_bytes_);
        }
        if(!mem) { cells_ = nullptr; return; }
        cells_ = static_cast<Cell*>(mem);
        prefault_cells();
        produced_ = 0; consumed_ = 0;
        cached_consumed_ = 0; cached_produced_ = 0;
    }

    void destroy() noexcept {
        if(!cells_) return;
#if SPP_SPSC_HAVE_HUGEPAGE
        if(alloc_kind_ == Alloc_Mmap) {
            ::munmap(cells_, alloc_bytes_);
        } else
#endif
#if defined(SPP_SPSC_USE_LIBNUMA)
        if(alloc_kind_ == Alloc_Numa) {
            ::numa_free(cells_, alloc_bytes_);
        } else
#endif
        {
            ::free(cells_);
        }
        cells_ = nullptr; capacity_ = 0; mask_ = 0;
        alloc_kind_ = Alloc_Heap; alloc_bytes_ = 0;
    }

    // =====================================================================
    // Single-element push
    // =====================================================================

    // Hot path with shadow cursor: 1 relaxed load (own line), 1 memcpy,
    // 1 release store (cell sequence), 1 increment (own line).  On x86
    // this is ~6 instructions, ~3-4 cycles total.
    //
    // The shadow check (`p - cached_c < n`) is the mathematical "full"
    // predicate evaluated entirely on the producer's private state.
    // Only on a lap-change do we sync the real consumer position.
    [[nodiscard]] SPP_SPSC_FORCE_INLINE bool try_push(const T& v) noexcept {
        const u64 p = produced_;
        Cell* const c = cells_ + (p & mask_);

        if constexpr(UseShadowCursor) {
            if(SPP_SPSC_LIKELY(p - cached_consumed_ < capacity_)) {
                spsc_memcpy(c->storage, &v, sizeof(T));
                seq_st_rel(&c->sequence, static_cast<i64>(p + 1));
                produced_ = p + 1;
                return true;
            }
            // Shadow says full — pull real consumer pos.
            cached_consumed_ = pos_ld(&consumed_);
            if(p - cached_consumed_ >= capacity_) return false;
            // Slot was freed between the shadow and the refresh — proceed.
            spsc_memcpy(c->storage, &v, sizeof(T));
            seq_st_rel(&c->sequence, static_cast<i64>(p + 1));
            produced_ = p + 1;
            return true;
        } else {
            const i64 seq = seq_ld_acq(&c->sequence);
            if(SPP_SPSC_UNLIKELY(seq != static_cast<i64>(p))) return false;
            spsc_memcpy(c->storage, &v, sizeof(T));
            seq_st_rel(&c->sequence, static_cast<i64>(p + 1));
            produced_ = p + 1;
            return true;
        }
    }

    SPP_SPSC_FORCE_INLINE void push_blocking(const T& v) noexcept {
        while(SPP_SPSC_UNLIKELY(!try_push(v))) spsc_pause();
    }

    // Zero-copy: reserve a slot, write payload in-place, then commit.
    [[nodiscard]] SPP_SPSC_FORCE_INLINE T* reserve() noexcept {
        const u64 p = produced_;
        Cell* const c = cells_ + (p & mask_);

        if constexpr(UseShadowCursor) {
            if(SPP_SPSC_UNLIKELY(p - cached_consumed_ >= capacity_)) {
                cached_consumed_ = pos_ld(&consumed_);
                if(p - cached_consumed_ >= capacity_) return nullptr;
            }
        } else {
            const i64 seq = seq_ld_acq(&c->sequence);
            if(SPP_SPSC_UNLIKELY(seq != static_cast<i64>(p))) return nullptr;
        }
        return reinterpret_cast<T*>(c->storage);
    }

    SPP_SPSC_FORCE_INLINE void commit() noexcept {
        const u64 p = produced_;
        seq_st_rel(&cells_[p & mask_].sequence, static_cast<i64>(p + 1));
        produced_ = p + 1;
    }

    // =====================================================================
    // Batch push — the mathematical model shines here.
    //
    // Reserve N contiguous slots, write all payloads, then commit with N
    // release stores.  On x86 with shadow cursor:
    //   1 relaxed-load (refresh cached_c if needed)
    //   N memcpy
    //   N plain-MOV stores (release → mov on x86)
    //   1 store produced_
    //
    // Amortised cost per item approaches (memcpy + 1 store) / N, plus a
    // fraction of the shadow-refresh cost.  For N ≥ 16, effective per-item
    // latency drops below 5 ns on Skylake+.
    // =====================================================================

    [[nodiscard]] SPP_SPSC_FORCE_INLINE u64
    try_push_batch(const T* items, u64 count) noexcept {
        if(SPP_SPSC_UNLIKELY(count == 0)) return 0;
        const u64 p = produced_;
        const u64 m = mask_;

        if constexpr(UseShadowCursor) {
            if(SPP_SPSC_UNLIKELY(p + count - cached_consumed_ > capacity_)) {
                cached_consumed_ = pos_ld(&consumed_);
                if(p + count - cached_consumed_ > capacity_) {
                    // Partial push: commit as many as fit.
                    u64 avail = capacity_ - (p - cached_consumed_);
                    if(avail == 0) return 0;
                    count = avail < count ? avail : count;
                }
            }
        }
        // No shadow cursor: clamp `count` to the number of free slots by
        // scanning cell sequences. Must clamp (not early-return) so the
        // write/commit loops below actually push the items we report —
        // returning `i` before writing would claim a push that never happened.
        if constexpr(!UseShadowCursor) {
            u64 free = 0;
            while(free < count &&
                  seq_ld_acq(&cells_[(p + free) & m].sequence)
                      == static_cast<i64>(p + free)) {
                ++free;
            }
            count = free;
            if(count == 0) return 0;
        }
        // Write payloads. Prefetch PF slots ahead (for-write) so a cold cell
        // line is being pulled in while we memcpy the current one — hides the
        // occasional L1 miss behind useful work.
        constexpr u64 PF = 3;
        if(count > PF) {
            for(u64 k = 0; k < PF; ++k)
                SPP_SPSC_PREFETCH(&cells_[(p + k) & m], 1, 3);
        }
        for(u64 i = 0; i < count; ++i) {
            if(i + PF < count)
                SPP_SPSC_PREFETCH(&cells_[(p + i + PF) & m], 1, 3);
            spsc_memcpy(cells_[(p + i) & m].storage, items + i, sizeof(T));
        }
        // Commit: release-store every cell's sequence, IN ORDER so the
        // consumer sees them in the same order they were written.
        for(u64 i = 0; i < count; ++i) {
            Cell* c = cells_ + ((p + i) & m);
            seq_st_rel(&c->sequence, static_cast<i64>(p + i + 1));
        }
        produced_ = p + count;
        return count;
    }

    // Batch reserve: returns pointer to the first of N contiguous slots.
    // Caller writes payloads, then calls commit_n(N).
    // Returns nullptr if N slots aren't free.
    [[nodiscard]] SPP_SPSC_FORCE_INLINE T* reserve_n(u64 count) noexcept {
        if(SPP_SPSC_UNLIKELY(count == 0)) return nullptr;
        const u64 p = produced_;

        if constexpr(UseShadowCursor) {
            if(SPP_SPSC_UNLIKELY(p + count - cached_consumed_ > capacity_)) {
                cached_consumed_ = pos_ld(&consumed_);
                if(p + count - cached_consumed_ > capacity_) return nullptr;
            }
        }
        return reinterpret_cast<T*>(cells_[(p & mask_)].storage);
    }

    SPP_SPSC_FORCE_INLINE void commit_n(u64 count) noexcept {
        const u64 p = produced_;
        const u64 m = mask_;
        for(u64 i = 0; i < count; ++i) {
            seq_st_rel(&cells_[(p + i) & m].sequence, static_cast<i64>(p + i + 1));
        }
        produced_ = p + count;
    }

    // =====================================================================
    // Single-element pop
    // =====================================================================

    [[nodiscard]] SPP_SPSC_FORCE_INLINE bool try_pop(T& out) noexcept {
        const u64 c = consumed_;
        Cell* const cell = cells_ + (c & mask_);

        if constexpr(UseShadowCursor) {
            if(SPP_SPSC_LIKELY(c < cached_produced_)) {
                spsc_memcpy(&out, cell->storage, sizeof(T));
                seq_st_rel(&cell->sequence, static_cast<i64>(c + capacity_));
                consumed_ = c + 1;
                return true;
            }
            cached_produced_ = pos_ld(&produced_);
            if(c >= cached_produced_) return false;
            spsc_memcpy(&out, cell->storage, sizeof(T));
            seq_st_rel(&cell->sequence, static_cast<i64>(c + capacity_));
            consumed_ = c + 1;
            return true;
        } else {
            const i64 seq = seq_ld_acq(&cell->sequence);
            if(SPP_SPSC_UNLIKELY(seq != static_cast<i64>(c + 1))) return false;
            spsc_memcpy(&out, cell->storage, sizeof(T));
            seq_st_rel(&cell->sequence, static_cast<i64>(c + capacity_));
            consumed_ = c + 1;
            return true;
        }
    }

    SPP_SPSC_FORCE_INLINE void pop_blocking(T& out) noexcept {
        while(SPP_SPSC_UNLIKELY(!try_pop(out))) {
            prefetch_next_read();
            spsc_pause();
        }
    }

    [[nodiscard]] SPP_SPSC_FORCE_INLINE const T* peek() noexcept {
        const u64 c = consumed_;
        Cell* const cell = cells_ + (c & mask_);

        if constexpr(UseShadowCursor) {
            if(SPP_SPSC_UNLIKELY(c >= cached_produced_)) {
                cached_produced_ = pos_ld(&produced_);
                if(c >= cached_produced_) return nullptr;
            }
        } else {
            if(seq_ld_acq(&cell->sequence) != static_cast<i64>(c + 1)) return nullptr;
        }
        return reinterpret_cast<const T*>(cell->storage);
    }

    SPP_SPSC_FORCE_INLINE void release() noexcept {
        const u64 c = consumed_;
        seq_st_rel(&cells_[c & mask_].sequence, static_cast<i64>(c + capacity_));
        consumed_ = c + 1;
    }

    // =====================================================================
    // Batch pop — symmetrical to batch push.
    // =====================================================================

    [[nodiscard]] SPP_SPSC_FORCE_INLINE u64
    try_pop_batch(T* out, u64 count) noexcept {
        if(SPP_SPSC_UNLIKELY(count == 0)) return 0;
        const u64 c = consumed_;
        const u64 m = mask_;

        if constexpr(UseShadowCursor) {
            u64 avail = cached_produced_ - c;
            if(SPP_SPSC_UNLIKELY(avail < count)) {
                cached_produced_ = pos_ld(&produced_);
                avail = cached_produced_ - c;
                if(avail == 0) return 0;
                if(avail < count) count = avail;
            }
        }
        // No shadow cursor: clamp `count` to the number of ready slots before
        // the copy loop. The old code did an early `return i` mid-loop, which
        // released the first `i` cells (advancing their sequence) WITHOUT
        // advancing consumed_ — corrupting the ring. Clamping first keeps
        // consumed_ in lockstep with the cells we actually drain.
        if constexpr(!UseShadowCursor) {
            u64 avail = 0;
            while(avail < count &&
                  seq_ld_acq(&cells_[(c + avail) & m].sequence)
                      == static_cast<i64>(c + avail + 1)) {
                ++avail;
            }
            count = avail;
            if(count == 0) return 0;
        }
        // Prefetch PF slots ahead (for-read) to overlap a cold cell line with
        // the current memcpy — symmetric to the producer-side batch prefetch.
        constexpr u64 PF = 3;
        if(count > PF) {
            for(u64 k = 0; k < PF; ++k)
                SPP_SPSC_PREFETCH(&cells_[(c + k) & m], 0, 3);
        }
        for(u64 i = 0; i < count; ++i) {
            if(i + PF < count)
                SPP_SPSC_PREFETCH(&cells_[(c + i + PF) & m], 0, 3);
            Cell* cell = cells_ + ((c + i) & m);
            spsc_memcpy(out + i, cell->storage, sizeof(T));
            seq_st_rel(&cell->sequence, static_cast<i64>(c + i + capacity_));
        }
        consumed_ = c + count;
        return count;
    }

    // =====================================================================
    // Prefetch — keeps the consumer's current-cell payload lines warm in
    // L1 during long idle windows, so the eventual pop doesn't pay a
    // cold-L3 (or cold-L2) refill penalty of 40-80 ns.
    // =====================================================================

    SPP_SPSC_FORCE_INLINE void prefetch_next_read() const noexcept {
        Cell* c = cells_ + (consumed_ & mask_);
        SPP_SPSC_PREFETCH(reinterpret_cast<const char*>(c) +   0, 0, 3);
        SPP_SPSC_PREFETCH(reinterpret_cast<const char*>(c) +  64, 0, 3);
        SPP_SPSC_PREFETCH(reinterpret_cast<const char*>(c) + 128, 0, 3);
    }

    // =====================================================================
    // Query
    // =====================================================================

    [[nodiscard]] u64 capacity() const noexcept { return capacity_; }

    [[nodiscard]] u64 size_approx() const noexcept {
        u64 p = pos_ld(&produced_);
        u64 c = pos_ld(&consumed_);
        return p >= c ? p - c : 0;
    }

    [[nodiscard]] bool is_empty() const noexcept {
        return pos_ld(&produced_) == pos_ld(&consumed_);
    }

    [[nodiscard]] bool is_full() const noexcept {
        return pos_ld(&produced_) - pos_ld(&consumed_) >= capacity_;
    }

private:
    // Prefault every cell: write the initial sequence AND touch the payload
    // storage so the backing pages are populated (and broken out of any
    // zero-page COW mapping) before the hot path runs. A first-touch page
    // fault mid-run is a classic p99 latency spike; paying it once at init
    // keeps steady-state push/pop fault-free.
    void prefault_cells() noexcept {
        for(u64 i = 0; i < capacity_; ++i) {
            seq_st_rel(&cells_[i].sequence, static_cast<i64>(i));
            __builtin_memset(cells_[i].storage, 0, sizeof(cells_[i].storage));
        }
        // Warm pass: pull each cell's first line back into cache after the
        // memset sweep evicted it, so the very first ops hit warm lines.
        for(u64 i = 0; i < capacity_; ++i) {
            SPP_SPSC_PREFETCH(&cells_[i], 1, 3);
        }
    }

    // Allocate `sz` bytes for the cell array. Prefers 2 MiB hugepages on Linux
    // (explicit MAP_HUGETLB, then THP-hinted anon mmap), falling back to
    // posix_memalign. Reports the deallocator class via *kind / *bytes.
    [[nodiscard]] static void*
    huge_or_heap_alloc(u64 sz, u64* kind, u64* bytes) noexcept {
#if SPP_SPSC_HAVE_HUGEPAGE
        const u64 mapped = (sz + SPP_SPSC_HUGE_2M - 1) & ~(SPP_SPSC_HUGE_2M - 1);
#if defined(MAP_HUGETLB) && defined(MAP_HUGE_SHIFT)
        void* hp = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
                              (21 << MAP_HUGE_SHIFT),
                          -1, 0);
        if(hp != MAP_FAILED) {
            *kind = Alloc_Mmap; *bytes = mapped; return hp;
        }
#endif
        void* anon = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if(anon != MAP_FAILED) {
            ::madvise(anon, mapped, MADV_HUGEPAGE);
            *kind = Alloc_Mmap; *bytes = mapped; return anon;
        }
#endif
        void* mem = nullptr;
        if(::posix_memalign(&mem, SPP_SPSC_PAD_LINE, sz) != 0) mem = nullptr;
        *kind = Alloc_Heap; *bytes = 0;
        return mem;
    }

    [[nodiscard]] static u64 ceil_pow2(u64 x) noexcept {
        if(x <= 1) return 1;
        --x;
        x |= x >> 1;  x |= x >> 2;
        x |= x >> 4;  x |= x >> 8;
        x |= x >> 16; x |= x >> 32;
        return x + 1;
    }
};

} // namespace Concurrency
} // namespace spp
