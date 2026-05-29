#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/concurrency/thread.h"

namespace spp::quant::risk {

// =========================================================================
// TokenBucket — standard token bucket rate limiter
//
// Tokens refill at a constant rate (tokens/second) up to a maximum
// capacity (burst_size). Each request consumes N tokens. If insufficient
// tokens are available, the request is denied.
//
// Thread safety: the bucket uses an internal mutex for all mutation.
// The can_consume() method is const and non-locking for hot-path reads.
//
// Usage:
//   TokenBucket bucket = TokenBucket::create(10.0, 20.0); // 10 rps, burst 20
//   if (bucket.consume(1)) { /* allowed */ }
// =========================================================================

struct TokenBucket {
    f64    max_tokens_      = 0.0;
    f64    tokens_          = 0.0;
    f64    refill_rate_     = 0.0;       // tokens per second
    u64    last_refill_us_  = 0;         // timestamp of last refill (microseconds)

    Thread::Mutex mutex_;

    // -------------------------------------------------------------------
    // create — factory for a token bucket
    //
    // max_rate_per_sec: sustained rate in tokens/second
    // burst_size:       maximum burst size (0 = same as rate, i.e., no burst)
    // -------------------------------------------------------------------
    static TokenBucket create(f64 max_rate_per_sec, f64 burst_size = 0.0) noexcept {
        TokenBucket tb;
        tb.refill_rate_ = max_rate_per_sec;
        tb.max_tokens_  = (burst_size > 0.0) ? burst_size : max_rate_per_sec;
        tb.tokens_      = tb.max_tokens_;  // start full
        tb.last_refill_us_ = now_us();
        return tb;
    }

    // -------------------------------------------------------------------
    // now_us — current monotonic timestamp in microseconds
    //
    // Uses Thread::perf_counter() for high-resolution, monotonic timing.
    // Falls back to std::chrono if perf frequency is unavailable.
    // -------------------------------------------------------------------
    static u64 now_us() noexcept {
        u64 freq = Thread::perf_frequency();
        if (freq > 0) {
            u64 counter = Thread::perf_counter();
            // counter * 1_000_000 / freq
            // Use f64 intermediate to avoid u64 overflow while preserving
            // sufficient precision for rate-limiting purposes (sub-ms accuracy).
            f64 counter_f = static_cast<f64>(counter);
            f64 freq_f    = static_cast<f64>(freq);
            return static_cast<u64>(counter_f * 1'000'000.0 / freq_f);
        }
        // Fallback: system without perf counter. Return 0 — the first
        // call will initialize last_refill_us_ and subsequent calls will
        // compute elapsed time correctly (all times will be 0, so no refill
        // occurs — effectively disables rate limiting as a safe default).
        return 0;
    }

    // -------------------------------------------------------------------
    // refill — add tokens based on elapsed time since last refill
    //
    // Must be called with mutex_ held OR from single-threaded context.
    // -------------------------------------------------------------------
    void refill() noexcept {
        u64 now = now_us();
        if (last_refill_us_ == 0) {
            last_refill_us_ = now;
            return;
        }

        u64 elapsed_us = (now > last_refill_us_) ? (now - last_refill_us_) : 0;
        if (elapsed_us == 0) return;

        // tokens_to_add = refill_rate * (elapsed_us / 1_000_000)
        // = refill_rate * elapsed_us / 1_000_000
        f64 tokens_to_add = refill_rate_ *
                            static_cast<f64>(elapsed_us) / 1'000'000.0;

        tokens_ += tokens_to_add;
        if (tokens_ > max_tokens_) {
            tokens_ = max_tokens_;
        }

        last_refill_us_ = now;
    }

    // -------------------------------------------------------------------
    // consume — try to consume `tokens` from the bucket
    //
    // Returns true if tokens were available and consumed.
    // Thread-safe.
    // -------------------------------------------------------------------
    bool consume(u64 tokens = 1) noexcept {
        Thread::Lock lock(mutex_);
        refill();

        f64 needed = static_cast<f64>(tokens);
        if (tokens_ >= needed) {
            tokens_ -= needed;
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------
    // can_consume — check without consuming (non-locking for hot-path)
    //
    // NOTE: This is a snapshot. Token state may change between this call
    // and the actual consume() call in a multi-threaded context.
    // Use consume() directly in production paths.
    // -------------------------------------------------------------------
    [[nodiscard]] bool can_consume(u64 tokens = 1) const noexcept {
        // Read without lock — approximate but fast
        f64 available = tokens_;
        u64 last_us   = last_refill_us_;
        u64 now       = now_us();

        // Estimate tokens at current time
        if (last_us > 0 && now > last_us) {
            f64 elapsed_sec = static_cast<f64>(now - last_us) / 1'000'000.0;
            available += refill_rate_ * elapsed_sec;
            if (available > max_tokens_) available = max_tokens_;
        }

        return available >= static_cast<f64>(tokens);
    }

    // -------------------------------------------------------------------
    // reset — reset bucket to full
    // -------------------------------------------------------------------
    void reset() noexcept {
        Thread::Lock lock(mutex_);
        tokens_         = max_tokens_;
        last_refill_us_ = now_us();
    }

    // -------------------------------------------------------------------
    // available_tokens — current token count (for monitoring)
    // -------------------------------------------------------------------
    [[nodiscard]] f64 available_tokens() const noexcept {
        return tokens_;
    }

    SPP_RECORD(TokenBucket,
               SPP_FIELD(max_tokens_),
               SPP_FIELD(tokens_),
               SPP_FIELD(refill_rate_),
               SPP_FIELD(last_refill_us_));
};

// =========================================================================
// ExchangeRateLimiter — multi-category rate limiter for exchange APIs
//
// Models the common exchange pattern where different API endpoint types
// have different rate limits, and a total weight limit caps all activity
// within a sliding window.
//
// Binance example:
//   - Order placement: 50 orders / 10 seconds
//   - Weight limit: 1200 per minute (orders cost 1-4 weight each)
//
// Each sub-bucket (order, cancel, REST, WebSocket) is a TokenBucket.
// The total weight limit tracks cumulative "cost" of all actions in a
// sliding window.
// =========================================================================

struct ExchangeRateLimiter {
    // ---- Sub-category buckets ----
    TokenBucket order_bucket_;
    TokenBucket cancel_bucket_;
    TokenBucket rest_bucket_;
    TokenBucket websocket_bucket_;

    // ---- Weight-based limit (Binance-style) ----
    f64   total_weight_limit_   = 1200.0;     // max weight per window
    f64   total_weight_used_    = 0.0;         // weight used in current window
    u64   window_start_us_      = 0;
    u64   window_duration_us_   = 60'000'000;  // 1 minute in microseconds

    Thread::Mutex mutex_;

    // =================================================================
    // Action enum — maps exchange operations to their bucket
    // =================================================================
    enum struct Action : u8 {
        PlaceOrder    = 0,
        CancelOrder   = 1,
        QueryPosition = 2,
        QueryBalance  = 3,
        Subscribe     = 4,
    };

    ExchangeRateLimiter() noexcept = default;

    // =================================================================
    // Pre-built configurations for major exchanges
    // =================================================================

    // Binance: 1200 weight / minute, orders ~1-4 weight each
    static ExchangeRateLimiter binance_limits() noexcept {
        ExchangeRateLimiter lim;
        // ~100 orders per 10 seconds, burst 20
        lim.order_bucket_     = TokenBucket::create(10.0, 20.0);
        // ~100 cancels per 10 seconds, burst 20
        lim.cancel_bucket_    = TokenBucket::create(10.0, 20.0);
        // ~1200 requests per minute, burst 100
        lim.rest_bucket_      = TokenBucket::create(20.0, 100.0);
        // ~5 subscriptions per second, burst 10
        lim.websocket_bucket_ = TokenBucket::create(5.0, 10.0);
        lim.total_weight_limit_ = 1200.0;
        return lim;
    }

    // OKX: general 20 req / 2 sec, orders 60 / 2 sec
    static ExchangeRateLimiter okx_limits() noexcept {
        ExchangeRateLimiter lim;
        // ~30 orders per second, burst 60
        lim.order_bucket_     = TokenBucket::create(30.0, 60.0);
        // ~30 cancels per second, burst 60
        lim.cancel_bucket_    = TokenBucket::create(30.0, 60.0);
        // ~10 REST requests per second, burst 20
        lim.rest_bucket_      = TokenBucket::create(10.0, 20.0);
        // ~5 subscriptions per second
        lim.websocket_bucket_ = TokenBucket::create(5.0, 10.0);
        lim.total_weight_limit_ = 600.0;
        return lim;
    }

    // Bybit: 50 orders / second
    static ExchangeRateLimiter bybit_limits() noexcept {
        ExchangeRateLimiter lim;
        // ~50 orders per second, burst 100
        lim.order_bucket_     = TokenBucket::create(50.0, 100.0);
        // ~50 cancels per second, burst 100
        lim.cancel_bucket_    = TokenBucket::create(50.0, 100.0);
        // ~50 REST requests per second
        lim.rest_bucket_      = TokenBucket::create(50.0, 100.0);
        // ~10 subscriptions per second
        lim.websocket_bucket_ = TokenBucket::create(10.0, 20.0);
        lim.total_weight_limit_ = 1000.0;
        return lim;
    }

private:
    // =================================================================
    // get_bucket — map action to its token bucket
    // =================================================================
    TokenBucket& get_bucket(Action action) noexcept {
        switch (action) {
        case Action::PlaceOrder:    return order_bucket_;
        case Action::CancelOrder:   return cancel_bucket_;
        case Action::QueryPosition: return rest_bucket_;
        case Action::QueryBalance:  return rest_bucket_;
        case Action::Subscribe:     return websocket_bucket_;
        default:                    return rest_bucket_;
        }
    }

    // =================================================================
    // check_weight — slide the weight window and check if action fits
    //
    // Returns true if the action's weight can be accommodated.
    // Does NOT consume the weight — that happens in execute().
    // =================================================================
    bool check_weight(u64 weight) noexcept {
        u64 now = TokenBucket::now_us();
        u64 window_elapsed = (now > window_start_us_) ? (now - window_start_us_) : 0;

        // If the window has expired, reset it
        if (window_elapsed >= window_duration_us_) {
            total_weight_used_ = 0.0;
            window_start_us_   = now;
        }

        f64 proposed = total_weight_used_ + static_cast<f64>(weight);
        return proposed <= total_weight_limit_;
    }

    // =================================================================
    // consume_weight — record the weight usage
    // =================================================================
    void consume_weight(u64 weight) noexcept {
        u64 now = TokenBucket::now_us();
        u64 window_elapsed = (now > window_start_us_) ? (now - window_start_us_) : 0;

        if (window_elapsed >= window_duration_us_) {
            total_weight_used_ = 0.0;
            window_start_us_   = now;
        }

        total_weight_used_ += static_cast<f64>(weight);
    }

public:
    // =================================================================
    // can_execute — check if an action is allowed without consuming
    //
    // Checks both the action-specific bucket AND the total weight limit.
    // Non-locking snapshot — use execute() for guaranteed consumption.
    // =================================================================
    [[nodiscard]] bool can_execute(Action action, u64 weight = 1) const noexcept {
        // Check sub-bucket (approximate, non-locking)
        TokenBucket& bucket = const_cast<ExchangeRateLimiter*>(this)->get_bucket(action);
        if (!bucket.can_consume(1)) return false;

        // Check weight limit (approximate)
        u64 now = TokenBucket::now_us();
        u64 window_elapsed = (now > window_start_us_) ? (now - window_start_us_) : 0;
        f64 used = total_weight_used_;
        if (window_elapsed >= window_duration_us_) {
            used = 0.0;
        }
        f64 proposed = used + static_cast<f64>(weight);
        return proposed <= total_weight_limit_;
    }

    // =================================================================
    // execute — consume resources for an action
    //
    // Returns true if the action is allowed and resources were consumed.
    // Returns false if rate limit would be exceeded (no consumption).
    // Thread-safe: acquires internal mutex.
    // =================================================================
    bool execute(Action action, u64 weight = 1) noexcept {
        Thread::Lock lock(mutex_);

        // 1. Check sub-bucket
        TokenBucket& bucket = get_bucket(action);
        bucket.refill();
        if (bucket.tokens_ < 1.0) return false;

        // 2. Check total weight
        if (!check_weight(weight)) return false;

        // 3. Consume both
        bucket.tokens_ -= 1.0;
        consume_weight(weight);

        return true;
    }

    // =================================================================
    // reset — reset all buckets and weight counters
    // =================================================================
    void reset() noexcept {
        Thread::Lock lock(mutex_);
        order_bucket_.reset();
        cancel_bucket_.reset();
        rest_bucket_.reset();
        websocket_bucket_.reset();
        total_weight_used_ = 0.0;
        window_start_us_   = TokenBucket::now_us();
    }

    // =================================================================
    // remaining_weight — weight remaining in the current window
    // =================================================================
    [[nodiscard]] f64 remaining_weight() const noexcept {
        u64 now = TokenBucket::now_us();
        u64 window_elapsed = (now > window_start_us_) ? (now - window_start_us_) : 0;
        if (window_elapsed >= window_duration_us_) {
            return total_weight_limit_;
        }
        f64 remaining = total_weight_limit_ - total_weight_used_;
        return (remaining >= 0.0) ? remaining : 0.0;
    }

    SPP_RECORD(ExchangeRateLimiter,
               SPP_FIELD(order_bucket_),
               SPP_FIELD(cancel_bucket_),
               SPP_FIELD(rest_bucket_),
               SPP_FIELD(websocket_bucket_),
               SPP_FIELD(total_weight_limit_),
               SPP_FIELD(total_weight_used_),
               SPP_FIELD(window_start_us_),
               SPP_FIELD(window_duration_us_));
};

// =========================================================================
// Throttler — enforces minimum interval between consecutive actions
//
// Spin-waits if needed to ensure the minimum interval. Use for simple
// pacing like "no more than 1 request every 100ms" without the overhead
// of a full token bucket.
//
// Thread safety: NOT thread-safe by default. Each thread should use its
// own Throttler instance.
// =========================================================================

struct Throttler {
    u64 min_interval_us_  = 0; // minimum microseconds between actions
    u64 last_action_us_   = 0;

    // -------------------------------------------------------------------
    // create — factory with minimum interval in microseconds
    // -------------------------------------------------------------------
    static Throttler create(u64 min_interval_us) noexcept {
        Throttler t;
        t.min_interval_us_ = min_interval_us;
        t.last_action_us_  = 0;
        return t;
    }

    // -------------------------------------------------------------------
    // wait — block until enough time has passed since last action
    //
    // If enough time has already passed, returns 0 immediately.
    // Otherwise, spin-waits (using Thread::pause() for efficiency) and
    // returns the actual wait time in microseconds.
    //
    // After returning, the timer is updated to now.
    // -------------------------------------------------------------------
    u64 wait() noexcept {
        u64 now = TokenBucket::now_us();

        if (last_action_us_ == 0) {
            last_action_us_ = now;
            return 0;
        }

        u64 elapsed = (now > last_action_us_) ? (now - last_action_us_) : 0;

        if (elapsed >= min_interval_us_) {
            last_action_us_ = now;
            return 0;
        }

        u64 remaining = min_interval_us_ - elapsed;

        // Spin-wait with pause for efficiency
        u64 waited = 0;
        while (waited < remaining) {
            // Check time every ~100us of spinning
            u64 spin_now = TokenBucket::now_us();
            u64 spin_elapsed = (spin_now > last_action_us_)
                                   ? (spin_now - last_action_us_)
                                   : 0;
            if (spin_elapsed >= min_interval_us_) break;

            Thread::pause(); // CPU-friendly hint
            waited = (spin_now > last_action_us_)
                         ? (spin_now - last_action_us_)
                         : 0;
        }

        last_action_us_ = TokenBucket::now_us();
        u64 actual_wait = (last_action_us_ > now) ? (last_action_us_ - now) : 0;
        return actual_wait;
    }

    // -------------------------------------------------------------------
    // can_act — check if enough time has passed without waiting
    // -------------------------------------------------------------------
    [[nodiscard]] bool can_act() const noexcept {
        u64 now = TokenBucket::now_us();
        if (last_action_us_ == 0) return true;
        u64 elapsed = (now > last_action_us_) ? (now - last_action_us_) : 0;
        return elapsed >= min_interval_us_;
    }

    // -------------------------------------------------------------------
    // mark_action — record an action without waiting
    //
    // Use when you know you've already waited (e.g., external timer).
    // -------------------------------------------------------------------
    void mark_action() noexcept {
        last_action_us_ = TokenBucket::now_us();
    }

    // -------------------------------------------------------------------
    // reset — clear the last action timestamp
    // -------------------------------------------------------------------
    void reset() noexcept {
        last_action_us_ = 0;
    }

    SPP_RECORD(Throttler,
               SPP_FIELD(min_interval_us_),
               SPP_FIELD(last_action_us_));
};

}  // namespace spp::quant::risk

SPP_NAMED_ENUM(::spp::quant::risk::ExchangeRateLimiter::Action, "ExchangeAction", PlaceOrder,
               SPP_CASE(PlaceOrder), SPP_CASE(CancelOrder), SPP_CASE(QueryPosition),
               SPP_CASE(QueryBalance), SPP_CASE(Subscribe));
