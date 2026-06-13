#include "test.h"

#include <spp/concurrency/spsc_ring.h>

using spp::Concurrency::Spsc_Ring;

template<bool Shadow>
static void basic_single() {
    Spsc_Ring<i64, Shadow> ring(8);
    assert(ring.capacity() == 8);
    assert(ring.size_approx() == 0);
    assert(ring.is_empty());
    assert(!ring.is_full());

    // Fill to capacity.
    for(i64 i = 0; i < 8; i++) {
        assert(ring.try_push(i));
    }
    assert(ring.size_approx() == 8);
    assert(ring.is_full());
    assert(!ring.is_empty());
    // Full: next push must fail.
    assert(!ring.try_push(99));

    // FIFO drain.
    for(i64 i = 0; i < 8; i++) {
        i64 out = -1;
        assert(ring.try_pop(out));
        assert(out == i);
    }
    // Empty: next pop must fail.
    i64 dummy = -1;
    assert(!ring.try_pop(dummy));
    assert(ring.size_approx() == 0);

    // Wrap-around: push/pop past one full lap to exercise the sequence lap math.
    i64 expect = 0;
    for(i64 round = 0; round < 5; round++) {
        for(i64 i = 0; i < 6; i++) {
            assert(ring.try_push(expect + i));
        }
        for(i64 i = 0; i < 6; i++) {
            i64 out = -1;
            assert(ring.try_pop(out));
            assert(out == expect + i);
        }
        expect += 6;
    }
}

template<bool Shadow>
static void zero_copy() {
    Spsc_Ring<i64, Shadow> ring(4);

    i64* slot = ring.reserve();
    assert(slot != nullptr);
    *slot = 0xABCD;
    ring.commit();

    const i64* p = ring.peek();
    assert(p != nullptr && *p == 0xABCD);
    ring.release();

    assert(ring.peek() == nullptr);
}

template<bool Shadow>
static void batch() {
    Spsc_Ring<i64, Shadow> ring(16);

    i64 src[10];
    for(i64 i = 0; i < 10; i++) src[i] = 1000 + i;

    u64 pushed = ring.try_push_batch(src, 10);
    assert(pushed == 10);
    assert(ring.size_approx() == 10);

    i64 dst[10] = {};
    u64 popped = ring.try_pop_batch(dst, 10);
    assert(popped == 10);
    for(i64 i = 0; i < 10; i++) assert(dst[i] == 1000 + i);

    // Over-request on a near-full ring: capacity 16, push 16, request 20 -> 16.
    i64 big[20];
    for(i64 i = 0; i < 20; i++) big[i] = i;
    u64 fit = ring.try_push_batch(big, 20);
    assert(fit == 16);
    i64 out[20] = {};
    u64 got = ring.try_pop_batch(out, 20);
    assert(got == 16);
    for(i64 i = 0; i < 16; i++) assert(out[i] == i);
}

// Single-producer / single-consumer threaded stress: the canonical use case.
// Validates strict FIFO ordering and that no item is lost or duplicated.
template<bool Shadow>
static void threaded_spsc() {
    constexpr i64 kCount = 200000;
    Spsc_Ring<i64, Shadow> ring(1024);

    auto producer = Thread::spawn([&ring]() mutable {
        for(i64 i = 0; i < kCount; i++) {
            ring.push_blocking(i);
        }
    });

    auto consumer = Thread::spawn([&ring]() mutable -> i64 {
        i64 next = 0;
        i64 out = -1;
        while(next < kCount) {
            if(ring.try_pop(out)) {
                assert(out == next);  // strict FIFO
                next++;
            } else {
                Thread::pause();
            }
        }
        return next;
    });

    producer->block();
    i64 consumed = consumer->block();
    assert(consumed == kCount);
}

i32 main() {
    Test test{"empty"_v};

    Trace("Spsc_Ring single-element (shadow)") { basic_single<true>(); }
    Trace("Spsc_Ring single-element (no-shadow)") { basic_single<false>(); }
    Trace("Spsc_Ring zero-copy (shadow)") { zero_copy<true>(); }
    Trace("Spsc_Ring zero-copy (no-shadow)") { zero_copy<false>(); }
    Trace("Spsc_Ring batch (shadow)") { batch<true>(); }
    Trace("Spsc_Ring batch (no-shadow)") { batch<false>(); }
    Trace("Spsc_Ring threaded SPSC (shadow)") { threaded_spsc<true>(); }
    Trace("Spsc_Ring threaded SPSC (no-shadow)") { threaded_spsc<false>(); }

    return 0;
}
