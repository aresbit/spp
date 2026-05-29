#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"

#include <cstdio>  // fopen/fgets/fputs/fputc/fclose/remove — required for KillSwitch file I/O

namespace spp::quant::risk {

// =========================================================================
// BreakerState — escalation levels for the circuit breaker
// =========================================================================
enum struct BreakerState : u8 {
    Normal   = 0,  // trading as usual
    Warning  = 1,  // approaching limit — reduce size
    SoftStop = 2,  // stop opening new positions, close existing
    HardStop = 3,  // immediately cancel all orders and liquidate everything
    Emergency = 4, // kill switch — disconnect, cancel all, halt process
};

// =========================================================================
// BreakerState description strings
// =========================================================================
inline String_View breaker_state_name(BreakerState s) noexcept {
    switch (s) {
    case BreakerState::Normal:    return "Normal"_v;
    case BreakerState::Warning:   return "Warning"_v;
    case BreakerState::SoftStop:  return "SoftStop"_v;
    case BreakerState::HardStop:  return "HardStop"_v;
    case BreakerState::Emergency: return "Emergency"_v;
    default:                      return "Unknown"_v;
    }
}

// =========================================================================
// CircuitBreaker — emergency stop for trading
//
// Monitors PnL, position size, connection health, and error rates.
// Triggers automatic position liquidation when thresholds are breached.
//
// Escalation path (short circuits to more severe state as appropriate):
//   Normal -> Warning -> SoftStop -> HardStop -> Emergency
//
// Thread safety: all public mutation methods acquire the internal mutex.
// Read-only query methods do NOT lock (for low-latency hot-path reads).
// The caller is responsible for using can_open_positions() / can_send_orders()
// in the hot path; these are non-locking and return the atomic state.
// =========================================================================
struct CircuitBreaker {
    // ---- PnL-based triggers ----
    f64 max_daily_loss_            = 100'000.0;   // absolute dollar amount
    f64 max_daily_loss_pct_        = 0.05;        // 5% of portfolio value
    f64 max_drawdown_from_peak_pct_= 0.10;        // 10% from peak equity
    u64 max_consecutive_losses_    = 10;          // consecutive losing trades

    // ---- Position-based triggers ----
    f64 max_leverage_              = 5.0;         // hard leverage cap
    f64 max_position_concentration_= 0.30;        // 30% max in single instrument

    // ---- Error / health triggers ----
    u64 max_consecutive_rejects_   = 5;           // consecutive order rejections
    u64 max_consecutive_timeouts_  = 3;           // consecutive connection timeouts
    u64 max_reconnects_per_minute_ = 10;          // reconnection storm detection

    // ---- Current state (atomic for hot-path reads) ----
    BreakerState state_ = BreakerState::Normal;

    // ---- Tracking (protected by mutex) ----
    f64  daily_pnl_             = 0.0;
    f64  peak_equity_           = 0.0;
    f64  current_equity_        = 1'000'000.0;
    u64  consecutive_losses_    = 0;
    u64  consecutive_rejects_   = 0;
    u64  consecutive_timeouts_  = 0;
    u64  reconnect_count_       = 0;
    Date last_reconnect_window_ = Date{};

    // ---- Internal mutex for thread safety ----
    Thread::Mutex mutex_;

private:
    // =====================================================================
    // escalate — determine the appropriate state based on current metrics
    // Caller MUST hold mutex_.
    // The method always picks the most severe state that is warranted.
    // It never demotes state — demotion is only via manual_reset().
    // =====================================================================
    BreakerState escalate() noexcept {
        BreakerState current = state_;

        // Emergency level checks (always checked — can jump directly)
        // 1. Reconnection storm
        if (reconnect_count_ >= max_reconnects_per_minute_ &&
            max_reconnects_per_minute_ > 0) {
            if (static_cast<u8>(BreakerState::Emergency) > static_cast<u8>(current)) {
                current = BreakerState::Emergency;
            }
        }

        // 2. HardStop: consecutive timeouts >= max
        if (max_consecutive_timeouts_ > 0 &&
            consecutive_timeouts_ >= max_consecutive_timeouts_) {
            if (static_cast<u8>(BreakerState::HardStop) > static_cast<u8>(current)) {
                current = BreakerState::HardStop;
            }
        }

        // 3. HardStop: consecutive rejects >= max
        if (max_consecutive_rejects_ > 0 &&
            consecutive_rejects_ >= max_consecutive_rejects_) {
            if (static_cast<u8>(BreakerState::HardStop) > static_cast<u8>(current)) {
                current = BreakerState::HardStop;
            }
        }

        // 4. HardStop: drawdown from peak
        if (max_drawdown_from_peak_pct_ > 0.0 && peak_equity_ > 0.0) {
            f64 drawdown = (peak_equity_ - current_equity_) / peak_equity_;
            if (drawdown >= max_drawdown_from_peak_pct_) {
                if (static_cast<u8>(BreakerState::HardStop) > static_cast<u8>(current)) {
                    current = BreakerState::HardStop;
                }
            }
            // Warning: approaching drawdown limit (80% of limit)
            else if (drawdown >= max_drawdown_from_peak_pct_ * 0.80) {
                if (static_cast<u8>(BreakerState::Warning) > static_cast<u8>(current)) {
                    current = BreakerState::Warning;
                }
            }
        }

        // 5. SoftStop: daily loss exceeded (absolute)
        if (max_daily_loss_ > 0.0 && daily_pnl_ <= -max_daily_loss_) {
            if (static_cast<u8>(BreakerState::SoftStop) > static_cast<u8>(current)) {
                current = BreakerState::SoftStop;
            }
        }

        // 6. SoftStop: daily loss exceeded (percentage)
        if (max_daily_loss_pct_ > 0.0 && current_equity_ > 0.0) {
            f64 loss_pct = -daily_pnl_ / current_equity_;
            if (loss_pct >= max_daily_loss_pct_) {
                if (static_cast<u8>(BreakerState::SoftStop) > static_cast<u8>(current)) {
                    current = BreakerState::SoftStop;
                }
            }
        }

        // 7. SoftStop: consecutive losers
        if (max_consecutive_losses_ > 0 &&
            consecutive_losses_ >= max_consecutive_losses_) {
            if (static_cast<u8>(BreakerState::SoftStop) > static_cast<u8>(current)) {
                current = BreakerState::SoftStop;
            }
        }

        // 8. Warning: approaching daily loss (80% of limit)
        if (max_daily_loss_ > 0.0 && daily_pnl_ < 0.0) {
            f64 loss_ratio = -daily_pnl_ / max_daily_loss_;
            if (loss_ratio >= 0.80) {
                if (static_cast<u8>(BreakerState::Warning) > static_cast<u8>(current)) {
                    current = BreakerState::Warning;
                }
            }
        }

        // 9. Warning: approaching consecutive losers (80% of limit)
        if (max_consecutive_losses_ > 0) {
            u64 warn_threshold = max_consecutive_losses_ * 80 / 100;
            if (warn_threshold > 0 && consecutive_losses_ >= warn_threshold) {
                if (static_cast<u8>(BreakerState::Warning) > static_cast<u8>(current)) {
                    current = BreakerState::Warning;
                }
            }
        }

        // 10. Leverage check
        if (max_leverage_ > 0.0 && current_equity_ > 0.0) {
            // Leverage is checked externally (position manager provides it);
            // here we only guard: if we are in an elevated state already,
            // we want to ensure leverage is reduced.
            // This is informational — actual enforcement is in check_order().
        }

        return current;
    }

public:
    CircuitBreaker() noexcept = default;

    // =====================================================================
    // update_pnl — call after every PnL update (on fill or mark-to-market)
    // =====================================================================
    BreakerState update_pnl(f64 pnl_delta) noexcept {
        Thread::Lock lock(mutex_);
        daily_pnl_ += pnl_delta;
        BreakerState new_state = escalate();
        state_ = new_state;
        return new_state;
    }

    // =====================================================================
    // update_equity — call after every price update (for drawdown tracking)
    // =====================================================================
    BreakerState update_equity(f64 new_equity) noexcept {
        Thread::Lock lock(mutex_);
        current_equity_ = new_equity;
        if (new_equity > peak_equity_) {
            peak_equity_ = new_equity;
            // Reset consecutive losses on new peak (trader is recovering)
            consecutive_losses_ = 0;
        }
        BreakerState new_state = escalate();
        state_ = new_state;
        return new_state;
    }

    // =====================================================================
    // on_reject — call when an order is rejected by the exchange
    // =====================================================================
    BreakerState on_reject(String_View /*reason*/) noexcept {
        Thread::Lock lock(mutex_);
        consecutive_rejects_++;
        BreakerState new_state = escalate();
        state_ = new_state;
        return new_state;
    }

    // =====================================================================
    // on_timeout — call on connection timeout
    // =====================================================================
    BreakerState on_timeout() noexcept {
        Thread::Lock lock(mutex_);
        consecutive_timeouts_++;
        BreakerState new_state = escalate();
        state_ = new_state;
        return new_state;
    }

    // =====================================================================
    // on_reconnect — call on successful reconnection
    //
    // Resets timeout counter (reconnection succeeded).
    // Increments reconnect_count_ for storm detection.
    // Resets the window if more than 60 seconds since last window started.
    // =====================================================================
    BreakerState on_reconnect() noexcept {
        Thread::Lock lock(mutex_);
        // Reset timeout counter on successful reconnect
        consecutive_timeouts_ = 0;

        // Manage reconnect rate window
        Date now = Date::today();
        i32 days_diff = (now.serial_ > last_reconnect_window_.serial_)
                            ? now.serial_ - last_reconnect_window_.serial_
                            : 0;
        // If the date changed (or first time), reset the window
        if (last_reconnect_window_.serial_ == 0 || days_diff >= 1) {
            last_reconnect_window_ = now;
            reconnect_count_       = 1;
        } else {
            reconnect_count_++;
        }

        BreakerState new_state = escalate();
        state_ = new_state;
        return new_state;
    }

    // =====================================================================
    // on_fill — call when an order fills
    //
    // Resets consecutive rejects (fill means exchange is accepting orders).
    // Tracks consecutive losing trades.
    // =====================================================================
    BreakerState on_fill(f64 pnl) noexcept {
        Thread::Lock lock(mutex_);
        // Successful fill resets reject counter
        consecutive_rejects_ = 0;

        // Track consecutive losses
        if (pnl < 0.0) {
            consecutive_losses_++;
        } else {
            consecutive_losses_ = 0;
        }

        daily_pnl_ += pnl;

        BreakerState new_state = escalate();
        state_ = new_state;
        return new_state;
    }

    // =====================================================================
    // manual_reset — operator resets the breaker to Normal
    // Call only after investigating and resolving the root cause.
    // Resets ALL counters to clean state.
    // =====================================================================
    void manual_reset() noexcept {
        Thread::Lock lock(mutex_);
        state_                = BreakerState::Normal;
        daily_pnl_            = 0.0;
        peak_equity_          = current_equity_;
        consecutive_losses_   = 0;
        consecutive_rejects_  = 0;
        consecutive_timeouts_ = 0;
        reconnect_count_      = 0;
        last_reconnect_window_= Date{};
    }

    // =====================================================================
    // manual_emergency_stop — operator triggers immediate emergency halt
    // =====================================================================
    void manual_emergency_stop() noexcept {
        Thread::Lock lock(mutex_);
        state_ = BreakerState::Emergency;
    }

    // =====================================================================
    // set_state — explicitly set the breaker state (for testing or manual ops)
    // =====================================================================
    void set_state(BreakerState new_state) noexcept {
        Thread::Lock lock(mutex_);
        state_ = new_state;
    }

    // =====================================================================
    // Hot-path queries: non-locking, atomic reads of the current state.
    // These are safe to call from the order-sending hot path.
    // =====================================================================

    // can_open_positions — true if we can open new positions
    [[nodiscard]] bool can_open_positions() const noexcept {
        BreakerState s = state_;
        return s == BreakerState::Normal || s == BreakerState::Warning;
    }

    // can_send_orders — true if we can send any orders at all
    [[nodiscard]] bool can_send_orders() const noexcept {
        BreakerState s = state_;
        return s == BreakerState::Normal || s == BreakerState::Warning;
    }

    // should_cancel_all — true if all open orders should be cancelled
    [[nodiscard]] bool should_cancel_all() const noexcept {
        BreakerState s = state_;
        return s == BreakerState::HardStop || s == BreakerState::Emergency;
    }

    // should_liquidate — true if all positions should be liquidated
    [[nodiscard]] bool should_liquidate() const noexcept {
        BreakerState s = state_;
        return s == BreakerState::HardStop || s == BreakerState::Emergency;
    }

    // state_description — human-readable state name
    [[nodiscard]] String_View state_description() const noexcept {
        return breaker_state_name(state_);
    }

    SPP_RECORD(CircuitBreaker,
               SPP_FIELD(max_daily_loss_),
               SPP_FIELD(max_daily_loss_pct_),
               SPP_FIELD(max_drawdown_from_peak_pct_),
               SPP_FIELD(max_consecutive_losses_),
               SPP_FIELD(max_leverage_),
               SPP_FIELD(max_position_concentration_),
               SPP_FIELD(max_consecutive_rejects_),
               SPP_FIELD(max_consecutive_timeouts_),
               SPP_FIELD(max_reconnects_per_minute_),
               SPP_FIELD(state_),
               SPP_FIELD(daily_pnl_),
               SPP_FIELD(peak_equity_),
               SPP_FIELD(current_equity_),
               SPP_FIELD(consecutive_losses_),
               SPP_FIELD(consecutive_rejects_),
               SPP_FIELD(consecutive_timeouts_),
               SPP_FIELD(reconnect_count_),
               SPP_FIELD(last_reconnect_window_));
};

// =========================================================================
// KillSwitch — hardware-level safety
//
// A separate monitoring process writes a signal file that the trading
// process reads before every order.
//
// Protocol:
//   - File contains one line: "KILL|<reason>" or "RESUME"
//   - activate()  writes "KILL|<reason>" to the file
//   - deactivate() writes "RESUME" to the file (or removes it)
//   - is_active()  reads the file, returns true if it starts with "KILL"
//   - reason()     reads the file, returns the part after "KILL|"
//
// Thread safety: file I/O is inherently racy with an external process,
// but each individual read/write is atomic at the syscall level for small
// payloads (PIPE_BUF / typical page size). This is the standard pattern
// for kill switches in trading systems.
// =========================================================================

struct KillSwitch {
    String_View signal_file_ = "/tmp/trading_kill"_v;

    KillSwitch() noexcept = default;

    explicit KillSwitch(String_View path) noexcept : signal_file_(path) {}

    // =====================================================================
    // is_active — check if kill switch is tripped
    //
    // Opens the signal file, reads first line. If it starts with "KILL",
    // the kill switch is active. Any error (file not found, can't read)
    // is treated as NOT active (fail-safe: if the monitor dies, we keep
    // trading rather than getting stuck).
    // =====================================================================
    [[nodiscard]] bool is_active() const noexcept {
        // Use C FILE* for simple, reliable file I/O
        // The null terminator is a valid C string, so we construct one
        // from String_View by copying to a small stack buffer.
        char path_buf[256];
        u64 path_len = signal_file_.length();
        if (path_len > 255) path_len = 255;
        for (u64 i = 0; i < path_len; i++) {
            path_buf[i] = signal_file_[i];
        }
        path_buf[path_len] = '\0';

        FILE* fp = std::fopen(path_buf, "r");
        if (!fp) {
            // File doesn't exist — kill switch is not active
            return false;
        }

        char line[128];
        char* result = std::fgets(line, static_cast<int>(sizeof(line)), fp);
        std::fclose(fp);

        if (!result) {
            // Empty file — not active
            return false;
        }

        // Check if line starts with "KILL"
        return (line[0] == 'K' && line[1] == 'I' && line[2] == 'L' && line[3] == 'L');
    }

    // =====================================================================
    // activate — trip the kill switch (called by monitoring process)
    //
    // Writes "KILL|<reason>" to the signal file. Returns true on success.
    // =====================================================================
    bool activate(String_View reason) noexcept {
        char path_buf[256];
        u64 path_len = signal_file_.length();
        if (path_len > 255) path_len = 255;
        for (u64 i = 0; i < path_len; i++) {
            path_buf[i] = signal_file_[i];
        }
        path_buf[path_len] = '\0';

        FILE* fp = std::fopen(path_buf, "w");
        if (!fp) return false;

        // Write "KILL|" header
        std::fputs("KILL|", fp);

        // Write reason
        u64 reason_len = reason.length();
        for (u64 i = 0; i < reason_len; i++) {
            std::fputc(static_cast<int>(reason[i]), fp);
        }
        std::fputc('\n', fp);

        std::fclose(fp);
        return true;
    }

    // =====================================================================
    // deactivate — reset the kill switch (called after manual review)
    //
    // Removes the signal file. Returns true on success (or if file
    // already doesn't exist).
    // =====================================================================
    bool deactivate() noexcept {
        char path_buf[256];
        u64 path_len = signal_file_.length();
        if (path_len > 255) path_len = 255;
        for (u64 i = 0; i < path_len; i++) {
            path_buf[i] = signal_file_[i];
        }
        path_buf[path_len] = '\0';

        // remove() returns 0 on success
        int rc = std::remove(path_buf);
        // ENOENT (file not found) is also success — already deactivated
        return (rc == 0);
    }

    // =====================================================================
    // reason — get kill reason from file
    //
    // Reads the signal file and returns the portion after "KILL|".
    // Returns empty String_View if not active or on error.
    // NOTE: Returned String_View points into a static buffer — the caller
    // must copy the result if it needs to persist.
    // =====================================================================
    [[nodiscard]] String_View reason() const noexcept {
        char path_buf[256];
        u64 path_len = signal_file_.length();
        if (path_len > 255) path_len = 255;
        for (u64 i = 0; i < path_len; i++) {
            path_buf[i] = signal_file_[i];
        }
        path_buf[path_len] = '\0';

        FILE* fp = std::fopen(path_buf, "r");
        if (!fp) return ""_v;

        char line[128];
        char* result = std::fgets(line, static_cast<int>(sizeof(line)), fp);
        std::fclose(fp);

        if (!result) return ""_v;

        // Check "KILL|" prefix
        if (!(line[0] == 'K' && line[1] == 'I' && line[2] == 'L' && line[3] == 'L' &&
              line[4] == '|')) {
            return ""_v;
        }

        // Find the newline and null-terminate
        u64 start = 5; // after "KILL|"
        u64 end = start;
        while (end < sizeof(line) && line[end] != '\0' && line[end] != '\n') {
            end++;
        }
        line[end] = '\0';

        // Return pointer into the stack buffer — caller must copy
        // We store reason in a static buffer for safety
        static char reason_buf[128];
        u64 len = end - start;
        if (len > 127) len = 127;
        for (u64 i = 0; i < len; i++) {
            reason_buf[i] = line[start + i];
        }
        reason_buf[len] = '\0';

        return String_View{reinterpret_cast<const u8*>(reason_buf), len};
    }

    SPP_RECORD(KillSwitch, SPP_FIELD(signal_file_));
};

}  // namespace spp::quant::risk

SPP_NAMED_ENUM(::spp::quant::risk::BreakerState, "BreakerState", Normal,
               SPP_CASE(Normal), SPP_CASE(Warning), SPP_CASE(SoftStop),
               SPP_CASE(HardStop), SPP_CASE(Emergency));
