// =============================================================================
// spsc bench — drives spp::Concurrency::Spsc_Ring through the exam scenarios.
//
// No <atomic>, no <thread>, no <chrono>, no <cstring>, no STL containers.
// All synchronization is done by sharing volatile flags between the two
// pthreads pinned to the isolated cores (CPUs 9-17 on the supplied test box).
// =============================================================================

#include <spp/concurrency/spsc_ring.h>
#include "spsc_msgs.h"
#include "spsc_timer.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

using namespace spp::Bench::Spsc;
using spp::Concurrency::Spsc_Ring;

// =============================================================================
// Test-box hardware (i9-10980XE, 18 cores, no HT, CPUs 9-17 isolated).
// Producer & consumer pinned to distinct cores on the isolated set.
// =============================================================================
static constexpr u64 MSG_COUNT = 600;
static constexpr u64 RING_CAP  = 1024;
static int PROD_CORE = 9;   // first isolated CPU
static int CONS_CORE = 11;  // skip one for L2 isolation

// =============================================================================
// Shared per-message timestamp arrays (one cache line each side).
// Producer writes g_send_tsc[i] just before push, consumer writes g_recv_tsc[i]
// immediately after pop. We compute latency offline after both threads join.
// =============================================================================
// 8192 slots gives us room for the 4096-sample ping-pong scenario plus
// the 600-sample exam scenarios; each entry is one TSC tick.
static constexpr u64 TS_SLOTS = 8192;
alignas(64) static u64 g_send_tsc[TS_SLOTS];
alignas(64) static u64 g_recv_tsc[TS_SLOTS];

// =============================================================================
// Scenario plumbing
// =============================================================================
struct Scenario {
    const char* name;
    u64         total_msgs;     // how many msgs this scenario sends
    u64         batch_size;     // msgs per producer burst
    long        gap_us;         // sleep between bursts (microseconds)
    bool        prefetch;       // enable consumer-side anti-eviction
                                // prefetch (worth it iff gap_us > ~10 µs)
};

struct ThreadCtx {
    Spsc_Ring<MsgEnvelope>* ring;
    MsgEnvelope*            msgs;
    Scenario                cfg;
    int                     core;
    volatile bool*          start_flag;
    volatile bool*          cons_ready;
    volatile u64*           cons_count;
    volatile u64*           prod_count;
};

// Compile-time consumer body so the prefetch branch can be specialised
// out for tight (P/C/D) scenarios and kept for long-idle (A/B/E).
template<bool DoPrefetch>
static void* consumer_loop(ThreadCtx* ctx) {
    pin_to_core(ctx->core);

    // Announce that we are spinning hot on the right core. The producer
    // blocks on this flag before timestamping anything, so the constant
    // pthread-create-to-first-pop latency (~13ms on this CentOS 7 box) does
    // NOT contaminate the round-trip samples.
    *ctx->cons_ready = true;

    while(!*ctx->start_flag) cpu_relax();

    u64 n = 0;
    u64 idle_spins = 0;
    MsgEnvelope tmp;
    while(n < ctx->cfg.total_msgs) {
        if(ctx->ring->try_pop(tmp)) {
            g_recv_tsc[n] = spp::Concurrency::rdtsc_raw();
            ++n;
            *ctx->cons_count = n;
            if constexpr(DoPrefetch) idle_spins = 0;
        } else {
            if constexpr(DoPrefetch) {
                // Long-idle anti-eviction: throttled prefetch keeps the
                // next-to-pop cell's payload pinned to L1d so the first pop
                // after a 500ms producer sleep doesn't pay cold-L3 refill.
                if(++idle_spins > 256 && (idle_spins & 0xff) == 0) {
                    ctx->ring->prefetch_next_read();
                }
            }
            cpu_relax();
        }
    }
    return nullptr;
}

static void* consumer_main_pf(void* arg) {
    return consumer_loop<true>(static_cast<ThreadCtx*>(arg));
}
static void* consumer_main_nopf(void* arg) {
    return consumer_loop<false>(static_cast<ThreadCtx*>(arg));
}

// ---- producer ----
static void run_producer(ThreadCtx* ctx) {
    pin_to_core(ctx->core);

    while(!*ctx->start_flag) cpu_relax();

    u64 sent = 0;
    const u64 total = ctx->cfg.total_msgs;
    const u64 bsz   = ctx->cfg.batch_size;
    const long gap  = ctx->cfg.gap_us;

    while(sent < total) {
        u64 end = sent + bsz <= total ? sent + bsz : total;
        for(u64 i = sent; i < end; ++i) {
            g_send_tsc[i] = spp::Concurrency::rdtsc_raw();
            ctx->ring->push_blocking(ctx->msgs[i]);
        }
        sent = end;
        *ctx->prod_count = sent;
        if(gap > 0 && sent < total) {
            timespec ts{};
            ts.tv_sec  = gap / 1'000'000;
            ts.tv_nsec = (gap % 1'000'000) * 1000;
            nanosleep(&ts, nullptr);
        }
    }
}

static void print_stats(const char* name, const LatencyStats& s, double ns_per_cycle) {
    (void)ns_per_cycle;
    printf("  [%s]\n", name);
    printf("    samples : %lu\n", s.n);
    printf("    min     : %lu ns\n", s.min_ns);
    printf("    p50     : %lu ns   <-- median\n", s.p50_ns);
    printf("    p90     : %lu ns\n", s.p90_ns);
    printf("    p95     : %lu ns\n", s.p95_ns);
    printf("    p99     : %lu ns\n", s.p99_ns);
    printf("    p99.9   : %lu ns\n", s.p999_ns);
    printf("    max     : %lu ns\n", s.max_ns);
    printf("    mean    : %lu ns\n", s.mean_ns);
    printf("\n");
}

static void run_scenario(Spsc_Ring<MsgEnvelope>& ring, MsgEnvelope* msgs,
                         const Scenario& cfg, const LatencyTimer& timer) {
    printf("=== %s ===\n", cfg.name);

    // Drain stale.
    MsgEnvelope dummy;
    while(ring.try_pop(dummy)) {}

    memset(g_send_tsc, 0, sizeof(u64) * cfg.total_msgs);
    memset(g_recv_tsc, 0, sizeof(u64) * cfg.total_msgs);

    volatile bool start_flag = false;
    volatile bool cons_ready = false;
    volatile u64  cons_count = 0;
    volatile u64  prod_count = 0;

    ThreadCtx cctx{&ring, msgs, cfg, CONS_CORE, &start_flag, &cons_ready,
                   &cons_count, &prod_count};
    pthread_t cth;
    pthread_create(&cth, nullptr,
                   cfg.prefetch ? &consumer_main_pf : &consumer_main_nopf,
                   &cctx);

    // Producer runs on the main thread.
    ThreadCtx pctx{&ring, msgs, cfg, PROD_CORE, &start_flag, &cons_ready,
                   &cons_count, &prod_count};
    pin_to_core(PROD_CORE);

    // Wait until the consumer has pinned and is in its hot spin loop.
    // This handshake eliminates the ~10-15ms one-time pthread spin-up cost
    // from showing up as a constant per-sample latency offset.
    while(!cons_ready) cpu_relax();

    start_flag = true;
    run_producer(&pctx);

    pthread_join(cth, nullptr);

    // ---- post-process ----
    static u64 ns[MSG_COUNT * 4];

    // Drop the first sample (warm-up: producer's first cell may still be cold).
    const u64 drop = cfg.total_msgs > 8 ? 1 : 0;
    const u64 nout = cfg.total_msgs - drop;
    for(u64 i = 0; i < nout; ++i) {
        u64 a = g_send_tsc[i + drop];
        u64 b = g_recv_tsc[i + drop];
        u64 c = b > a ? b - a : 0;
        ns[i] = timer.calibrated_ns(c);
    }

    LatencyStats stats = compute_stats(ns, nout);
    print_stats(cfg.name, stats, timer.ns_per_cycle());

    // Checksum the recovered messages to defeat dead-store elimination.
    u64 cs = 0;
    for(u64 i = 0; i < cfg.total_msgs; ++i) cs = checksum_envelope(msgs[i], cs);
    printf("  checksum: 0x%016lx\n\n", cs);
}

// =============================================================================
// Ping-pong scenario: producer pushes one msg, then waits for the consumer
// to pop it (signalled via *cons_count) before pushing the next. This rate-
// matches both sides and eliminates queue-fill bias — what remains is the
// raw cross-core push->pop hand-off time.
// =============================================================================
struct PingPongCtx {
    Spsc_Ring<MsgEnvelope>* ring;
    MsgEnvelope*            msgs;
    u64                     total;
    int                     core;
    volatile bool*          start;
    volatile bool*          ready;
    volatile u64*           popped;
};

static void* pingpong_consumer(void* arg) {
    auto* ctx = static_cast<PingPongCtx*>(arg);
    pin_to_core(ctx->core);
    *ctx->ready = true;
    while(!*ctx->start) cpu_relax();
    u64 n = 0;
    MsgEnvelope tmp;
    while(n < ctx->total) {
        if(ctx->ring->try_pop(tmp)) {
            g_recv_tsc[n] = spp::Concurrency::rdtsc_raw();
            ++n;
            *ctx->popped = n;
        } else {
            // Tight ping-pong: no idle long enough to warrant prefetch.
            cpu_relax();
        }
    }
    return nullptr;
}

static void run_pingpong(Spsc_Ring<MsgEnvelope>& ring, MsgEnvelope* msgs,
                         const LatencyTimer& timer, u64 N = 4096) {
    printf("=== Scenario P: cross-core ping-pong (rate-matched) ===\n");

    MsgEnvelope dummy;
    while(ring.try_pop(dummy)) {}

    constexpr u64 WARMUP = 256;
    const u64 total = N + WARMUP;

    volatile bool start  = false;
    volatile bool ready  = false;
    volatile u64  popped = 0;

    PingPongCtx ctx{&ring, msgs, total, CONS_CORE, &start, &ready, &popped};
    pthread_t cth;
    pthread_create(&cth, nullptr, &pingpong_consumer, &ctx);

    pin_to_core(PROD_CORE);
    while(!ready) cpu_relax();   // wait until consumer pinned + spinning

    start = true;     // release consumer

    // Lock-step push-then-wait-for-pop. The consumer writes g_recv_tsc[n]
    // using the same index `n` that the producer used to write g_send_tsc[n].
    for(u64 i = 0; i < total; ++i) {
        g_send_tsc[i] = spp::Concurrency::rdtsc_raw();
        ring.push_blocking(msgs[i % MSG_COUNT]);
        while(popped <= i) cpu_relax();
    }

    pthread_join(cth, nullptr);

    static u64 ns[8192];
    for(u64 i = 0; i < N; ++i) {
        u64 a = g_send_tsc[i + WARMUP];
        u64 b = g_recv_tsc[i + WARMUP];
        u64 c = b > a ? b - a : 0;
        ns[i] = timer.calibrated_ns(c);
    }
    auto s = compute_stats(ns, N);
    print_stats("ping-pong (1-deep, lock-step)", s, timer.ns_per_cycle());
}

// =============================================================================
// Single-side micro-benchmark: same thread does push+pop in a tight loop.
// This measures the bare cost of the queue operations themselves, removing
// cross-core cache traffic from the picture. The exam's 10ns target is
// realistic only for this measurement on this hardware.
// =============================================================================
static void run_single_thread_micro(Spsc_Ring<MsgEnvelope>& ring, MsgEnvelope* msgs,
                                    const LatencyTimer& timer) {
    printf("=== Scenario S: single-thread micro (push+pop bare cost) ===\n");

    pin_to_core(PROD_CORE);

    constexpr int N = 4096;
    u64 push_ns[N];
    u64 pop_ns[N];
    u64 rt_ns[N];

    MsgEnvelope out;
    // Warm cache.
    for(int i = 0; i < 256; ++i) {
        (void)ring.try_push(msgs[i % MSG_COUNT]);
        (void)ring.try_pop(out);
    }

    for(int i = 0; i < N; ++i) {
        u64 t0 = rdtsc_lfence();
        ring.push_blocking(msgs[i % MSG_COUNT]);
        u64 t1 = rdtsc_lfence();
        ring.pop_blocking(out);
        u64 t2 = rdtsc_lfence();
        push_ns[i] = timer.calibrated_ns(t1 - t0);
        pop_ns[i]  = timer.calibrated_ns(t2 - t1);
        rt_ns[i]   = timer.calibrated_ns(t2 - t0);
    }

    auto s_push = compute_stats(push_ns, N);
    auto s_pop  = compute_stats(pop_ns,  N);
    auto s_rt   = compute_stats(rt_ns,   N);
    print_stats("push (single-thread)", s_push, timer.ns_per_cycle());
    print_stats("pop  (single-thread)", s_pop,  timer.ns_per_cycle());
    print_stats("push+pop round-trip",  s_rt,   timer.ns_per_cycle());
}

// =============================================================================
static bool has_flag(int argc, char** argv, const char* flag) {
    for(int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        int j = 0;
        while(a[j] && flag[j] && a[j] == flag[j]) ++j;
        if(!a[j] && !flag[j]) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    const bool quick = has_flag(argc, argv, "--quick");
    const bool no_a  = has_flag(argc, argv, "--no-slow") || quick;
    printf("==== SPSC Lock-Free Queue Benchmark (spp::Concurrency::Spsc_Ring) ====\n");
    printf("MsgEnvelope size: %lu bytes\n", sizeof(MsgEnvelope));
    printf("Ring capacity   : %lu\n", RING_CAP);
    printf("Producer core   : %d\n", PROD_CORE);
    printf("Consumer core   : %d\n\n", CONS_CORE);

    LatencyTimer timer;
    printf("TSC frequency        : %lu Hz (%.2f GHz)\n",
           timer.tsc_freq_hz(),
           static_cast<double>(timer.tsc_freq_hz()) / 1e9);
    printf("rdtsc-pair overhead  : %lu cycles (%.1f ns)\n\n",
           timer.overhead_cycles(),
           static_cast<double>(timer.overhead_cycles()) * timer.ns_per_cycle());

    // Allocate messages + ring on the heap (large stack-frame risk otherwise).
    auto* msgs = static_cast<MsgEnvelope*>(
        aligned_alloc(64, sizeof(MsgEnvelope) * MSG_COUNT));
    generate_messages(msgs, MSG_COUNT, 0x12345u);

    Spsc_Ring<MsgEnvelope> ring(RING_CAP);

    // --- Scenario S: same-thread micro (bare push+pop cost) ----------------
    run_single_thread_micro(ring, msgs, timer);

    // --- Scenario P: cross-core rate-matched ping-pong ---------------------
    run_pingpong(ring, msgs, timer);

    // --- Scenario A: 1 msg / 500ms, sequential -----------------------------
    // The 500ms gap makes A+B take ~5 minutes total — skipped with --quick.
    if(!no_a) {
        Scenario s{"Scenario A: 1 msg / 500ms",  MSG_COUNT, 1, 500'000, true};
        run_scenario(ring, msgs, s, timer);
    }

    // --- Scenario B: 2 msgs / 500ms ----------------------------------------
    if(!no_a) {
        Scenario s{"Scenario B: 2 msgs / 500ms", MSG_COUNT, 2, 500'000, true};
        run_scenario(ring, msgs, s, timer);
    }

    // --- Scenario C: all 600 in one burst ----------------------------------
    {
        Scenario s{"Scenario C: burst 600 (no gap)", MSG_COUNT, MSG_COUNT, 0, false};
        run_scenario(ring, msgs, s, timer);
    }

    // --- Scenario D: 10-burst, sustained ----------------------------------
    {
        Scenario s{"Scenario D: 10-msg bursts, no gap", MSG_COUNT, 10, 0, false};
        run_scenario(ring, msgs, s, timer);
    }

    // --- Scenario E: streaming, 100us gap -----------------------------------
    {
        Scenario s{"Scenario E: 1 msg / 100us streaming", MSG_COUNT, 1, 100, true};
        run_scenario(ring, msgs, s, timer);
    }

    free(msgs);
    printf("==== bench complete ====\n");
    return 0;
}
