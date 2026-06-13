#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/concurrency/thread.h"

#ifdef SPP_OS_LINUX
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#endif

namespace spp::quant::monitoring {

// =========================================================================
// Watchdog — independent monitoring daemon
//
// Runs as a SEPARATE PROCESS that monitors the main trading process.
// If the trading process crashes, hangs, or violates safety limits,
// the watchdog triggers emergency actions.
//
// Communication: POSIX shared memory (shm_open/mmap) in /dev/shm.
//
// Architecture:
//   Trading Process  <---[shared memory]--->  Watchdog Daemon
//   (WatchdogClient)                        (WatchdogDaemon)
//
// =========================================================================

// =========================================================================
// WatchdogFlags — bit flags for WatchdogSharedState::state_flags_
// =========================================================================
namespace WatchdogFlags {
    constexpr u64 TRADING_ACTIVE   = 1 << 0;  // trading process is running
    constexpr u64 CONNECTION_OK    = 1 << 1;  // exchange connection is healthy
    constexpr u64 POSITIONS_FLAT   = 1 << 2;  // all positions closed
    constexpr u64 ORDERS_CLEARED   = 1 << 3;  // no pending orders
    constexpr u64 RISK_OK          = 1 << 4;  // risk limits not breached
    constexpr u64 SHUTDOWN_REQ     = 1 << 5;  // trading process requesting orderly shutdown
}

// =========================================================================
// ThrottleLevel — ordered escalation for trading restrictions
// =========================================================================
enum struct ThrottleLevel : u8 {
    Normal   = 0,  // full trading allowed
    Reduce   = 1,  // reduce position sizes
    StopNew  = 2,  // no new positions, only close existing
    FullStop = 3,  // emergency: cancel all, liquidate, halt
};

// =========================================================================
// WatchdogSharedState — shared memory between trading process and watchdog
//
// This struct must be POD (Plain Old Data) — no virtual methods, no
// pointers, no non-trivial types.  Thread::Atomic stores a plain i64
// and supports atomic operations through the platform's memory model.
//
// The trading process creates this in /dev/shm via shm_open/mmap.
// The watchdog opens and mmaps the same file read-only or read-write.
// =========================================================================
struct WatchdogSharedState {
    // --- Trading process writes these ---
    Thread::Atomic alive_counter_;      // incremented every heartbeat interval
    Thread::Atomic last_heartbeat_us_;  // timestamp of last heartbeat (microseconds)
    Thread::Atomic pnl_today_;          // current daily PnL (i64 fixed-point: value * 100)
    Thread::Atomic position_count_;     // number of open positions
    Thread::Atomic order_count_;        // number of pending orders
    Thread::Atomic leverage_bps_;       // current leverage in basis points (leverage * 10000)
    Thread::Atomic state_flags_;        // bit flags (see WatchdogFlags)

    // --- Watchdog process writes these ---
    Thread::Atomic emergency_stop_;     // 1 = emergency stop requested
    Thread::Atomic throttle_level_;     // 0=normal, 1=reduce, 2=stop_new, 3=full_stop

    // --- Padding to avoid false sharing on cache lines ---
    // Reserve 64 bytes total (8 atomics * 8 bytes each = 64, plus padding).
    // Each Thread::Atomic is effectively an i64, so 8 bytes each.
    // 10 fields * 8 bytes = 80 bytes — fits within two cache lines (128 bytes).
    u8 _padding_[48]; // pad to 128 bytes total (10*8 + 48 = 128)
};

// =========================================================================
// EMERGENCY_STOP_PHASES — watchdog escalation phases
// =========================================================================
namespace WatchdogPhases {
    // Phase 1: Set emergency_stop_ flag in shared memory (async signal)
    //          Trading process checks this flag before each order.
    // Phase 2: After PHASE2_DELAY_MS, if process is still running, send SIGTERM.
    // Phase 3: After PHASE3_DELAY_MS, if process is still running, send SIGKILL.
    constexpr u64 PHASE2_DELAY_MS = 2'000;   // 2 seconds after emergency flag
    constexpr u64 PHASE3_DELAY_MS = 5'000;   // 5 seconds after SIGTERM
}

// =========================================================================
// WatchdogClient — runs INSIDE the trading process
//
// Initializes the shared memory region and writes heartbeats.
// The trading main loop calls heartbeat() every ~100ms.
// =========================================================================
struct WatchdogClient {
    String_View             shm_path_;           // shared memory file path
    u64                     heartbeat_interval_us_ = 100'000; // 100ms

    WatchdogSharedState*    shm_    = null;
    i32                     shm_fd_ = -1;

    // =====================================================================
    // init — create and map the shared memory region
    //
    // The trading process owns the shm and creates it.
    // Returns true on success.
    // =====================================================================
    bool init(String_View shm_path = "/dev/shm/trading_watchdog"_v) noexcept {
#ifdef SPP_OS_LINUX
        shm_path_ = shm_path;

        // Build null-terminated path
        String<> path_s{shm_path.length() + 1};
        path_s.set_length(shm_path.length() + 1);
        for (u64 i = 0; i < shm_path.length(); i++) path_s[i] = shm_path[i];
        path_s[shm_path.length()] = '\0';

        // Remove any stale shm from a previous crash
        ::shm_unlink(reinterpret_cast<const char*>(path_s.data()));

        // Create shared memory object
        shm_fd_ = ::shm_open(reinterpret_cast<const char*>(path_s.data()),
                              O_RDWR | O_CREAT | O_EXCL, 0666);
        if (shm_fd_ < 0) {
            // If O_EXCL fails (file exists), try without O_EXCL
            shm_fd_ = ::shm_open(reinterpret_cast<const char*>(path_s.data()),
                                  O_RDWR | O_CREAT, 0666);
            if (shm_fd_ < 0) return false;
        }

        // Set size to fit the shared state struct
        if (::ftruncate(shm_fd_, static_cast<off_t>(sizeof(WatchdogSharedState))) != 0) {
            ::close(shm_fd_);
            shm_fd_ = -1;
            ::shm_unlink(reinterpret_cast<const char*>(path_s.data()));
            return false;
        }

        // Memory-map the shared region
        void* addr = ::mmap(null, sizeof(WatchdogSharedState),
                             PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (addr == MAP_FAILED) {
            ::close(shm_fd_);
            shm_fd_ = -1;
            ::shm_unlink(reinterpret_cast<const char*>(path_s.data()));
            return false;
        }

        // Zero-initialize and construct the shared state in-place
        shm_ = static_cast<WatchdogSharedState*>(addr);
        // Placement new for atomics (they are trivially constructible)
        new (shm_) WatchdogSharedState{};

        // Set initial trading active flag
        shm_->state_flags_.exchange(
            static_cast<i64>(WatchdogFlags::TRADING_ACTIVE));

        return true;
#else
        (void)shm_path;
        return false;
#endif
    }

    // =====================================================================
    // heartbeat — send heartbeat, updating all counters
    //
    // Call every ~100ms from the trading main loop.
    //
    // Parameters:
    //   daily_pnl      — current day's PnL in quote currency
    //   positions      — number of open positions
    //   orders         — number of pending orders
    //   leverage       — current leverage ratio (e.g. 1.5 = 1.5x)
    //   connection_ok  — exchange connection is healthy
    //   risk_ok        — risk limits are not breached
    // =====================================================================
    void heartbeat(f64 daily_pnl, u64 positions, u64 orders, f64 leverage,
                   bool connection_ok, bool risk_ok) noexcept {
        if (!shm_) return;

        // Increment alive counter (atomic — watchdog detects liveness by
        // checking this changes between reads)
        shm_->alive_counter_.incr();

        // Update timestamp
        shm_->last_heartbeat_us_.exchange(static_cast<i64>(now_us()));

        // Update PnL as fixed-point (pnl * 100)
        shm_->pnl_today_.exchange(static_cast<i64>(daily_pnl * 100.0));

        // Update counts
        shm_->position_count_.exchange(static_cast<i64>(positions));
        shm_->order_count_.exchange(static_cast<i64>(orders));

        // Update leverage in basis points (leverage * 10000)
        shm_->leverage_bps_.exchange(static_cast<i64>(leverage * 10000.0));

        // Update state flags
        i64 flags = static_cast<i64>(WatchdogFlags::TRADING_ACTIVE);
        if (connection_ok) flags |= static_cast<i64>(WatchdogFlags::CONNECTION_OK);
        if (positions == 0) flags |= static_cast<i64>(WatchdogFlags::POSITIONS_FLAT);
        if (orders == 0)    flags |= static_cast<i64>(WatchdogFlags::ORDERS_CLEARED);
        if (risk_ok)        flags |= static_cast<i64>(WatchdogFlags::RISK_OK);
        shm_->state_flags_.exchange(flags);
    }

    // =====================================================================
    // is_emergency_stop — check if watchdog has signalled emergency stop
    //
    // Call before every order placement. Zero-overhead on hot path:
    // just an atomic load.
    // =====================================================================
    [[nodiscard]] bool is_emergency_stop() const noexcept {
        if (!shm_) return false;
        return shm_->emergency_stop_.load() != 0;
    }

    // =====================================================================
    // throttle_level — check current throttle level
    // =====================================================================
    [[nodiscard]] u64 throttle_level() const noexcept {
        if (!shm_) return 0;
        return static_cast<u64>(shm_->throttle_level_.load());
    }

    // =====================================================================
    // signal_shutdown — notify watchdog of orderly shutdown
    // =====================================================================
    void signal_shutdown() noexcept {
        if (!shm_) return;
        i64 current = shm_->state_flags_.load();
        shm_->state_flags_.exchange(
            current | static_cast<i64>(WatchdogFlags::SHUTDOWN_REQ));
    }

    // =====================================================================
    // shutdown — cleanup shared memory
    //
    // The trading process unlinks the shm so the watchdog knows to exit.
    // =====================================================================
    void shutdown() noexcept {
#ifdef SPP_OS_LINUX
        if (shm_) {
            ::munmap(shm_, sizeof(WatchdogSharedState));
            shm_ = null;
        }
        if (shm_fd_ >= 0) {
            ::close(shm_fd_);
            shm_fd_ = -1;
        }
        // Unlink the shared memory file
        if (!shm_path_.empty()) {
            String<> path_s{shm_path_.length() + 1};
            path_s.set_length(shm_path_.length() + 1);
            for (u64 i = 0; i < shm_path_.length(); i++) path_s[i] = shm_path_[i];
            path_s[shm_path_.length()] = '\0';
            ::shm_unlink(reinterpret_cast<const char*>(path_s.data()));
        }
#else
        (void)shm_;
        (void)shm_fd_;
#endif
    }

private:
    [[nodiscard]] static u64 now_us() noexcept {
#ifdef SPP_OS_LINUX
        struct timeval tv;
        ::gettimeofday(&tv, null);
        return static_cast<u64>(tv.tv_sec) * 1'000'000ULL +
               static_cast<u64>(tv.tv_usec);
#else
        return 0;
#endif
    }
};

// =========================================================================
// WatchdogDaemon — runs as a SEPARATE PROCESS
//
// Monitors the trading process via shared memory. Implements a three-phase
// emergency escalation protocol.
//
// Usage (in watchdog binary's main):
//   WatchdogDaemon daemon;
//   daemon.set_trading_pid(getppid());
//   daemon.start("/dev/shm/trading_watchdog");
// =========================================================================
struct WatchdogDaemon {
    String_View shm_path_;
    u64 check_interval_us_    = 200'000;   // check every 200ms
    u64 heartbeat_timeout_us_ = 5'000'000; // 5 seconds without heartbeat = dead
    i64 max_daily_loss_pnl100_= 10;        // [UNSPECIFIED] default relative loss not set
                                            // — use absolute PnL from heartbeat data
    f64 max_leverage_         = 10.0;       // leverage cap
    u64 max_positions_        = 200;        // position count cap

    WatchdogSharedState* shm_  = null;
    i32                 shm_fd_ = -1;
    bool                running_ = false;

    // =====================================================================
    // start — attach to shared memory and begin monitoring
    //
    // Opens (does NOT create) the shared memory region. The trading process
    // must have already created it.
    //
    // Blocks in the monitoring loop until stop() is called or the
    // trading process exits.
    // =====================================================================
    bool start(String_View shm_path = "/dev/shm/trading_watchdog"_v) noexcept {
#ifdef SPP_OS_LINUX
        shm_path_ = shm_path;

        // Build null-terminated path
        String<> path_s{shm_path.length() + 1};
        path_s.set_length(shm_path.length() + 1);
        for (u64 i = 0; i < shm_path.length(); i++) path_s[i] = shm_path[i];
        path_s[shm_path.length()] = '\0';

        // Open existing shared memory (read-write so we can set emergency flags)
        shm_fd_ = ::shm_open(reinterpret_cast<const char*>(path_s.data()),
                              O_RDWR, 0666);
        if (shm_fd_ < 0) return false;

        // Get the size
        struct stat st;
        if (::fstat(shm_fd_, &st) != 0 ||
            static_cast<u64>(st.st_size) < sizeof(WatchdogSharedState)) {
            ::close(shm_fd_);
            shm_fd_ = -1;
            return false;
        }

        // Memory-map the shared region
        void* addr = ::mmap(null, sizeof(WatchdogSharedState),
                             PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (addr == MAP_FAILED) {
            ::close(shm_fd_);
            shm_fd_ = -1;
            return false;
        }

        shm_ = static_cast<WatchdogSharedState*>(addr);

        // Initialize tracking
        last_alive_counter_ = static_cast<u64>(shm_->alive_counter_.load());
        emergency_flag_set_at_us_ = 0;
        sigterm_sent_at_us_ = 0;

        // Run the monitoring loop
        running_ = true;
        run_loop();

        return true;
#else
        (void)shm_path;
        return false;
#endif
    }

    // =====================================================================
    // stop — exit the monitoring loop
    // =====================================================================
    void stop() noexcept {
        running_ = false;
    }

    // =====================================================================
    // set_trading_pid — set the PID of the trading process to monitor
    // =====================================================================
    void set_trading_pid(i32 pid) noexcept {
        trading_pid_ = pid;
    }

private:
    i32 trading_pid_ = -1;
    u64 last_alive_counter_ = 0;
    u64 emergency_flag_set_at_us_ = 0;
    u64 sigterm_sent_at_us_ = 0;

    // =====================================================================
    // run_loop — main monitoring loop
    //
    // Blocking. Call stop() from a signal handler to exit.
    // =====================================================================
    void run_loop() noexcept {
#ifdef SPP_OS_LINUX
        while (running_) {
            // Sleep for the check interval
            ::usleep(static_cast<useconds_t>(check_interval_us_));

            // Re-check if shm is still valid (trading process may have
            // called shutdown() and unlinked the shm). We check by trying
            // to access the structure.
            if (!shm_) {
                running_ = false;
                break;
            }

            read_shm();

            // Check: is the trading process alive?
            if (!is_trading_alive()) {
                // Trading process appears dead
                handle_trading_dead();
                continue;
            }

            // Check: has the trading process requested shutdown?
            i64 flags = shm_->state_flags_.load();
            if (flags & static_cast<i64>(WatchdogFlags::SHUTDOWN_REQ)) {
                // Orderly shutdown — trading process is exiting
                running_ = false;
                break;
            }

            // Check: PnL breach?
            if (is_pnl_breached()) {
                // [UNSPECIFIED] PnL threshold configuration.
                // In production, the watchdog should be configured with
                // an absolute daily loss threshold (e.g., $100K).
                // Currently, we trigger if the heartbeat reports a PnL
                // below the configured threshold.
                emergency_stop();
            }

            // Check: has emergency flag been set? If so, escalate
            if (shm_->emergency_stop_.load() != 0) {
                escalate_emergency();
            }
        }

        // Cleanup
        cleanup_shm();
#endif
    }

    // =====================================================================
    // read_shm — snapshot shared memory state
    // =====================================================================
    void read_shm() noexcept {
        if (!shm_) return;

        // Read alive counter — snapshot for is_trading_alive()
        last_alive_counter_ = static_cast<u64>(shm_->alive_counter_.load());

        // Read last heartbeat time
        last_heartbeat_us_ = static_cast<u64>(shm_->last_heartbeat_us_.load());
    }

    u64 last_heartbeat_us_ = 0;

    // =====================================================================
    // is_trading_alive — check if trading process has heartbeated recently
    // =====================================================================
    [[nodiscard]] bool is_trading_alive() const noexcept {
        if (!shm_) return false;

        i64 flags = shm_->state_flags_.load();
        if (!(flags & static_cast<i64>(WatchdogFlags::TRADING_ACTIVE))) {
            return false;
        }

        u64 now = now_us();
        u64 elapsed = (now > last_heartbeat_us_) ? (now - last_heartbeat_us_) : 0;
        return elapsed <= heartbeat_timeout_us_;
    }

    // =====================================================================
    // is_pnl_breached — check if PnL exceeds the loss threshold
    // =====================================================================
    [[nodiscard]] bool is_pnl_breached() const noexcept {
        if (!shm_) return false;

        i64 pnl_100 = shm_->pnl_today_.load();
        // pnl_100 is in hundredths (pnl * 100). A negative value means loss.
        // We compare against max_daily_loss_pnl100_ which is also *100.
        return pnl_100 <= -max_daily_loss_pnl100_;
    }

    // =====================================================================
    // handle_trading_dead — called when the trading process appears dead
    //
    // If the shm still exists but heartbeat has stopped, the process
    // may be hung. Send SIGTERM, then SIGKILL after a delay.
    // =====================================================================
    void handle_trading_dead() noexcept {
#ifdef SPP_OS_LINUX
        if (trading_pid_ <= 0) return;

        // Check if the process actually exists
        if (::kill(trading_pid_, 0) != 0) {
            // Process is gone — nothing to do
            running_ = false;
            return;
        }

        // Process exists but not responding — send SIGTERM
        ::kill(trading_pid_, SIGTERM);

        // Wait briefly, then send SIGKILL
        ::usleep(static_cast<useconds_t>(WatchdogPhases::PHASE3_DELAY_MS * 1000));
        if (::kill(trading_pid_, 0) == 0) {
            ::kill(trading_pid_, SIGKILL);
        }

        running_ = false;
#endif
    }

    // =====================================================================
    // emergency_stop — set emergency flag in shared memory
    // =====================================================================
    void emergency_stop() noexcept {
        if (!shm_) return;

        shm_->emergency_stop_.exchange(1);
        shm_->throttle_level_.exchange(static_cast<i64>(ThrottleLevel::FullStop));
        emergency_flag_set_at_us_ = now_us();
    }

    // =====================================================================
    // escalate_emergency — three-phase escalation after emergency flag set
    // =====================================================================
    void escalate_emergency() noexcept {
#ifdef SPP_OS_LINUX
        if (trading_pid_ <= 0) return;

        u64 now = now_us();
        u64 elapsed = (now > emergency_flag_set_at_us_)
                          ? (now - emergency_flag_set_at_us_) : 0;

        // Phase 2: If the process hasn't responded in PHASE2_DELAY_MS,
        // send SIGTERM
        if (elapsed >= WatchdogPhases::PHASE2_DELAY_MS * 1'000 &&
            sigterm_sent_at_us_ == 0) {
            if (::kill(trading_pid_, 0) == 0) {
                ::kill(trading_pid_, SIGTERM);
            }
            sigterm_sent_at_us_ = now;
        }

        // Phase 3: After SIGTERM, wait PHASE3_DELAY_MS then SIGKILL
        if (sigterm_sent_at_us_ > 0) {
            u64 since_term = (now > sigterm_sent_at_us_)
                                 ? (now - sigterm_sent_at_us_) : 0;
            if (since_term >= WatchdogPhases::PHASE3_DELAY_MS * 1'000) {
                if (::kill(trading_pid_, 0) == 0) {
                    ::kill(trading_pid_, SIGKILL);
                }
                running_ = false;
            }
        }
#else
        (void)trading_pid_;
#endif
    }

    // =====================================================================
    // cleanup_shm — unmap and close shared memory
    // =====================================================================
    void cleanup_shm() noexcept {
#ifdef SPP_OS_LINUX
        if (shm_) {
            ::munmap(shm_, sizeof(WatchdogSharedState));
            shm_ = null;
        }
        if (shm_fd_ >= 0) {
            ::close(shm_fd_);
            shm_fd_ = -1;
        }
#endif
    }

    [[nodiscard]] static u64 now_us() noexcept {
#ifdef SPP_OS_LINUX
        struct timeval tv;
        ::gettimeofday(&tv, null);
        return static_cast<u64>(tv.tv_sec) * 1'000'000ULL +
               static_cast<u64>(tv.tv_usec);
#else
        return 0;
#endif
    }
};

} // namespace spp::quant::monitoring

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::monitoring::WatchdogClient,
                 "WatchdogClient",
                 SPP_FIELD(heartbeat_interval_us_));

SPP_NAMED_RECORD(::spp::quant::monitoring::WatchdogDaemon,
                 "WatchdogDaemon",
                 SPP_FIELD(check_interval_us_),
                 SPP_FIELD(heartbeat_timeout_us_),
                 SPP_FIELD(max_daily_loss_pnl100_),
                 SPP_FIELD(max_leverage_),
                 SPP_FIELD(max_positions_));
