#pragma once

// =============================================================================
// rdtsc-based timer with TSC frequency calibration and overhead subtraction.
// Only depends on the kernel ABI (clock_gettime, pthread). No <chrono>,
// no <thread>, no <atomic>.
// =============================================================================

#include <spp/concurrency/spsc_ring.h>

#include <time.h>      // clock_gettime
#include <pthread.h>   // pthread_self / pthread_setaffinity_np
#include <sched.h>     // CPU_SET, cpu_set_t
#include <stdlib.h>    // qsort

namespace spp {
namespace Bench {
namespace Spsc {

using ::spp::Concurrency::u64;
using ::spp::Concurrency::i64;
using ::spp::Concurrency::rdtsc_lfence;
using ::spp::Concurrency::rdtsc_raw;
using ::spp::Concurrency::cpu_relax;

// ------------------------------------------------------------------
// Pin the calling thread to a single core.  Returns 0 on success.
// ------------------------------------------------------------------
inline int pin_to_core(int core) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// ------------------------------------------------------------------
// Stats container + percentile helper.
// ------------------------------------------------------------------
struct LatencyStats {
    u64 n;
    u64 min_ns, max_ns, mean_ns;
    u64 p50_ns, p90_ns, p95_ns, p99_ns, p999_ns;
};

static int cmp_u64(const void* a, const void* b) {
    u64 x = *static_cast<const u64*>(a);
    u64 y = *static_cast<const u64*>(b);
    return (x > y) - (x < y);
}

inline LatencyStats compute_stats(u64* samples, u64 n) noexcept {
    LatencyStats s{};
    s.n = n;
    if(n == 0) return s;
    qsort(samples, n, sizeof(u64), cmp_u64);
    s.min_ns = samples[0];
    s.max_ns = samples[n - 1];
    s.p50_ns  = samples[(u64)(n * 0.50)];
    s.p90_ns  = samples[(u64)(n * 0.90)];
    s.p95_ns  = samples[(u64)(n * 0.95)];
    s.p99_ns  = samples[(u64)(n * 0.99) < n ? (u64)(n * 0.99) : n - 1];
    s.p999_ns = samples[(u64)(n * 0.999) < n ? (u64)(n * 0.999) : n - 1];
    u64 sum = 0;
    for(u64 i = 0; i < n; ++i) sum += samples[i];
    s.mean_ns = sum / n;
    return s;
}

// ------------------------------------------------------------------
// LatencyTimer: TSC frequency + rdtsc-pair overhead self-calibration.
// ------------------------------------------------------------------
struct LatencyTimer {

    LatencyTimer() noexcept { calibrate(); }

    [[nodiscard]] u64 tsc_freq_hz() const noexcept { return freq_hz_; }
    [[nodiscard]] double ns_per_cycle() const noexcept { return ns_per_cycle_; }

    [[nodiscard]] u64 overhead_cycles() const noexcept { return overhead_cycles_; }

    [[nodiscard]] u64 cycles_to_ns(u64 cycles) const noexcept {
        return static_cast<u64>(static_cast<double>(cycles) * ns_per_cycle_);
    }

    // Subtract self-calibrated rdtsc-pair overhead.
    [[nodiscard]] u64 calibrated_cycles(u64 raw) const noexcept {
        return raw > overhead_cycles_ ? raw - overhead_cycles_ : 0;
    }

    [[nodiscard]] u64 calibrated_ns(u64 raw_cycles) const noexcept {
        return cycles_to_ns(calibrated_cycles(raw_cycles));
    }

private:
    void calibrate() noexcept {
        // ---- TSC frequency: rdtsc bracketed by CLOCK_MONOTONIC over 100ms.
        constexpr long cal_ns = 100'000'000L;  // 100ms
        u64 best = 0;
        for(int r = 0; r < 5; ++r) {
            timespec t0{}, t1{};
            clock_gettime(CLOCK_MONOTONIC, &t0);
            u64 c0 = rdtsc_raw();
            for(;;) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long d = (t1.tv_sec - t0.tv_sec) * 1'000'000'000L
                       + (t1.tv_nsec - t0.tv_nsec);
                if(d >= cal_ns) break;
            }
            u64 c1 = rdtsc_raw();
            long d = (t1.tv_sec - t0.tv_sec) * 1'000'000'000L
                   + (t1.tv_nsec - t0.tv_nsec);
            if(d > 0) {
                u64 f = static_cast<u64>(
                    static_cast<double>(c1 - c0) * 1e9 / static_cast<double>(d));
                if(f > best) best = f;
            }
        }
        freq_hz_      = best ? best : 3'000'000'000ULL;
        ns_per_cycle_ = 1e9 / static_cast<double>(freq_hz_);

        // ---- rdtsc-pair overhead: median of 4096 back-to-back rdtsc calls.
        constexpr int N = 4096;
        u64 samples[N];
        for(int i = 0; i < N; ++i) {
            u64 a = rdtsc_lfence();
            __asm__ __volatile__("" ::: "memory");
            u64 b = rdtsc_lfence();
            samples[i] = b - a;
        }
        qsort(samples, N, sizeof(u64), cmp_u64);
        overhead_cycles_ = samples[N / 2];
    }

    u64    freq_hz_         = 3'000'000'000ULL;
    double ns_per_cycle_    = 1.0 / 3.0;
    u64    overhead_cycles_ = 0;
};

} // namespace Spsc
} // namespace Bench
} // namespace spp
