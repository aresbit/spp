// =============================================================================
// SPSC latency micro-benchmark — validates the 20-40 ns per-op claim.
//
// Build:  cd bench/spsc && make latency_micro
// Run:    sudo ./build/latency_micro [producer_core] [consumer_core]
//
// Strategy: pin producer & consumer to two CPUs on the SAME physical core
// (SMT siblings, e.g. cpu 4 & 5 on Skylake-X) so the cell sequence
// round-trips through L1d (no snoop, no L3, no uncore traffic).
//
// Expected result on Skylake-X / Cascade Lake-X at 3.5+ GHz:
//   single-element round-trip:  ~24-38 ns  (~80-130 TSC cycles)
//   batch-8 amortised:          ~6-12 ns  (~20-40 TSC cycles/item)
// =============================================================================

#include <spp/concurrency/spsc_ring.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

using spp::Concurrency::Spsc_Ring;
using spp::Concurrency::u64;
using spp::Concurrency::spsc_pause;
using spp::Concurrency::i64;

// ---- Config ----------------------------------------------------------------
static constexpr u64 WARMUP   = 100'000;      // warmup iterations
static constexpr u64 SAMPLES  = 2'000'000;     // measured iterations
static constexpr u64 RING_CAP = 256;           // power-of-two capacity
static constexpr u64 CACHE_LINE = 64;

// ---- Per-batch prototype ---------------------------------------------------
struct alignas(64) Batch_Sample {
    u64 send_tsc;    // producer TSC just before commit_n
    u64 recv_tsc;    // consumer TSC just after try_pop_batch return
};
static Batch_Sample g_samples[(SAMPLES / 8) + 1];  // max batch slot count

// ---- Payload (8 bytes fits in one register; tests raw ring overhead) --------
struct Msg { u64 v; };

// ---- Shared control --------------------------------------------------------
static volatile int g_prod_ready = 0;
static volatile int g_cons_ready = 0;

// ---- Helpers ---------------------------------------------------------------
static u64 rdtsc() noexcept {
    unsigned lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<u64>(hi) << 32) | lo;
}

static void pin_cpu(int cpu) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

static u64 ns_from_tsc(u64 tsc, double tsc_per_ns) noexcept {
    return static_cast<u64>(static_cast<double>(tsc) / tsc_per_ns);
}

// ---- Producer thread -------------------------------------------------------
struct Prod_Args { Spsc_Ring<Msg>* ring; u64 samples; u64 batch; };

static void* producer_thread(void* arg) noexcept {
    Prod_Args* a = static_cast<Prod_Args*>(arg);
    Spsc_Ring<Msg>& ring = *a->ring;
    const u64 batch = a->batch;

    // Warmup
    for(u64 i = 0; i < WARMUP; ++i) {
        ring.try_push(Msg{i});
        while(!ring.try_push(Msg{i})) spsc_pause();
    }

    // Drain warmup
    { Msg dummy;
      for(u64 i = 0; i < WARMUP + batch; ++i) while(!ring.try_pop(dummy)); }

    __atomic_store_n(&g_prod_ready, 1, __ATOMIC_RELEASE);
    while(!__atomic_load_n(&g_cons_ready, __ATOMIC_ACQUIRE));

    for(u64 i = 0; i < a->samples; i += batch) {
        // Single-element path.
        if(batch == 1) {
            for(u64 j = 0; j < 1; ++j) {
                g_samples[i + j].send_tsc = rdtsc();
                ring.try_push(Msg{j});
            }
        } else {
            // Batch path.
            // Reserve + write payloads first.
            Msg* slots = ring.reserve_n(batch);
            if(!slots) {
                // Ring full — wait and retry.
                while(!(slots = ring.reserve_n(batch))) spsc_pause();
            }
            for(u64 j = 0; j < batch; ++j) slots[j].v = j;
            g_samples[i / batch].send_tsc = rdtsc();
            ring.commit_n(batch);
        }
    }
    return nullptr;
}

// ---- Consumer thread -------------------------------------------------------
struct Cons_Args { Spsc_Ring<Msg>* ring; u64 samples; u64 batch; };

static void* consumer_thread(void* arg) noexcept {
    Cons_Args* a = static_cast<Cons_Args*>(arg);
    Spsc_Ring<Msg>& ring = *a->ring;

    // Warmup drain
    while(!__atomic_load_n(&g_prod_ready, __ATOMIC_ACQUIRE));
    __atomic_store_n(&g_cons_ready, 1, __ATOMIC_RELEASE);

    for(u64 i = 0; i < a->samples; i += a->batch) {
        if(a->batch == 1) {
            Msg m;
            while(!ring.try_pop(m)) spsc_pause();
            g_samples[i].recv_tsc = rdtsc();
        } else {
            Msg buf[64];  // stack buffer; batch ≤ 64
            u64 got = 0;
            while((got = ring.try_pop_batch(buf, a->batch)) == 0)
                spsc_pause();
            g_samples[i / a->batch].recv_tsc = rdtsc();
        }
    }
    return nullptr;
}

// ---- Main ------------------------------------------------------------------
int main(int argc, char** argv) {
    int pcpu = argc > 1 ? atoi(argv[1]) : 4;
    int ccpu = argc > 2 ? atoi(argv[2]) : 5;

    // Calibrate TSC → ns.
    u64 t0 = rdtsc();
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    sleep(1);
    struct timespec ts1; clock_gettime(CLOCK_MONOTONIC, &ts1);
    u64 t1 = rdtsc();
    u64 ns_elapsed = (u64)(ts1.tv_sec - ts0.tv_sec) * 1'000'000'000ULL
                     + (u64)(ts1.tv_nsec - ts0.tv_nsec);
    double tsc_per_ns = static_cast<double>(t1 - t0) / static_cast<double>(ns_elapsed);

    printf("calibration: %.3f TSC/ns  (%.1f GHz nominal)\n",
           tsc_per_ns, tsc_per_ns);

    printf("\n%6s %8s %8s %8s %8s %8s\n",
           "batch", "avg_ns", "p50_ns", "p99_ns", "p999_ns", "max_ns");

    const u64 batches[] = {1,2,4,8,16,32};
    for(u64 bi = 0; bi < sizeof(batches)/sizeof(batches[0]); ++bi) {
        u64 batch = batches[bi];
        Spsc_Ring<Msg> ring(RING_CAP);
        u64 nsamples = SAMPLES / batch;
        if(nsamples * batch != SAMPLES) nsamples--;  // round down

        Prod_Args pa = {&ring, nsamples, batch};
        Cons_Args ca = {&ring, nsamples, batch};

        pthread_t pt, ct;
        pin_cpu(pcpu);
        pthread_create(&pt, nullptr, producer_thread, &pa);
        pin_cpu(ccpu);
        pthread_create(&ct, nullptr, consumer_thread, &ca);

        pthread_join(pt, nullptr);
        pthread_join(ct, nullptr);

        // Compute statistics.
        u64 sum  = 0, count = 0;
        u64 max_lat = 0;
        u64 hist[nsamples];
        for(u64 i = 0; i < nsamples; ++i) {
            u64 send = g_samples[i].send_tsc;
            u64 recv = g_samples[i].recv_tsc;
            if(send == 0 || recv == 0) continue;
            u64 lat_tsc = (recv > send) ? (recv - send) : 0;
            u64 lat_ns  = ns_from_tsc(lat_tsc, tsc_per_ns);
            sum += lat_ns;
            hist[count++] = lat_ns;
            if(lat_ns > max_lat) max_lat = lat_ns;
        }

        // Simple sort for percentiles.
        auto cmp = [](const void* a, const void* b) {
            u64 x = *(const u64*)a, y = *(const u64*)b;
            return x < y ? -1 : (x > y ? 1 : 0);
        };
        qsort(hist, count, sizeof(u64), cmp);

        u64 avg   = count ? sum / count : 0;
        u64 p50   = count ? hist[count * 50 / 100] : 0;
        u64 p99   = count ? hist[count * 99 / 100] : 0;
        u64 p999  = count ? hist[count * 999 / 1000] : 0;

        printf("%6llu %8llu %8llu %8llu %8llu %8llu\n",
               (unsigned long long)batch,
               (unsigned long long)avg,
               (unsigned long long)p50,
               (unsigned long long)p99,
               (unsigned long long)p999,
               (unsigned long long)max_lat);
    }
    return 0;
}
