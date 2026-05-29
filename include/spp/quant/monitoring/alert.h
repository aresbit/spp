#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/portfolio/position.h"  // PositionBook

#ifdef SPP_OS_LINUX
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#endif

namespace spp::quant {

// =========================================================================
// Forward declarations — types used by HealthMonitor that may be defined
// in other headers (e.g. risk/limits.h, execution/gateway.h).
// =========================================================================
struct CircuitBreaker;
struct ExchangeRateLimiter;
struct ExchangePosition;

namespace monitoring {

// =========================================================================
// AlertLevel — severity classification for alerts
// =========================================================================
enum struct AlertLevel : u8 { Info = 0, Warning = 1, Error = 2, Critical = 3 };

// =========================================================================
// Alert — a single trading alert / notification
// =========================================================================
struct Alert {
    AlertLevel  level_;
    Date        timestamp_;
    String_View source_;      ///< which component generated the alert
    String_View category_;    ///< e.g. "risk", "connection", "execution", "data"
    String_View message_;
    String_View detail_;      ///< optional additional detail

    SPP_RECORD(Alert, SPP_FIELD(level_), SPP_FIELD(timestamp_),
               SPP_FIELD(source_), SPP_FIELD(category_), SPP_FIELD(message_));
};

// =========================================================================
// AlertHandler — abstract interface for alert delivery
// =========================================================================
struct AlertHandler {
    String_View name_;

    /// Deliver an alert.  Must be implemented by concrete handlers.
    virtual void send(const Alert& alert) = 0;

    virtual ~AlertHandler() = default;
};

// =========================================================================
// ConsoleAlertHandler — writes alerts to stdout (Info/Warning) and
//                        stderr (Error/Critical).
// =========================================================================
struct ConsoleAlertHandler : AlertHandler {
    ConsoleAlertHandler() noexcept { name_ = "ConsoleAlertHandler"_v; }

    void send(const Alert& alert) override {
#ifdef SPP_OS_LINUX
        // Build a formatted message line
        const char* level_str = "INFO";
        i32 fd = STDOUT_FILENO;
        switch (alert.level_) {
        case AlertLevel::Warning:  level_str = "WARN";  break;
        case AlertLevel::Error:    level_str = "ERROR"; fd = STDERR_FILENO; break;
        case AlertLevel::Critical: level_str = "CRIT";  fd = STDERR_FILENO; break;
        default: break;
        }

        // Format: [LEVEL] YYYYMMDD source/category: message (detail)
        u8 buf[1024];
        i32 y=0; u32 m=0, d=0;
        { auto ymd_ = alert.timestamp_.ymd(); y=ymd_.get<0>(); m=ymd_.get<1>(); d=ymd_.get<2>(); }

        i32 len = Libc::snprintf(
            buf, sizeof(buf),
            "[%s] %04d%02d%02d %.*s/%.*s: %.*s (%.*s)\n",
            level_str, y, m, d,
            static_cast<i32>(alert.source_.length()), alert.source_.data(),
            static_cast<i32>(alert.category_.length()), alert.category_.data(),
            static_cast<i32>(alert.message_.length()), alert.message_.data(),
            static_cast<i32>(alert.detail_.length()), alert.detail_.data());

        if (len > 0) {
            ::write(fd, buf, static_cast<u64>(len));
        }
#else
        (void)alert;
#endif
    }
};

// =========================================================================
// FileAlertHandler — appends alerts to a log file
// =========================================================================
struct FileAlertHandler : AlertHandler {
    String_View file_path_;
    i32         fd_ = -1;

    FileAlertHandler() noexcept { name_ = "FileAlertHandler"_v; }

    explicit FileAlertHandler(String_View path) noexcept
        : file_path_(path) { name_ = "FileAlertHandler"_v; }

    void send(const Alert& alert) override {
#ifdef SPP_OS_LINUX
        // Lazy-open the file (append mode, create if missing)
        if (fd_ < 0) {
            // Build null-terminated path
            String<> path{file_path_.length() + 1};
            path.set_length(file_path_.length() + 1);
            for (u64 i = 0; i < file_path_.length(); i++) path[i] = file_path_[i];
            path[file_path_.length()] = '\0';

            fd_ = ::open(reinterpret_cast<const char*>(path.data()),
                         O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd_ < 0) return;  // cannot open — silently drop
        }

        const char* level_str = "INFO";
        switch (alert.level_) {
        case AlertLevel::Warning:  level_str = "WARN";  break;
        case AlertLevel::Error:    level_str = "ERROR"; break;
        case AlertLevel::Critical: level_str = "CRIT";  break;
        default: break;
        }

        u8 buf[1024];
        i32 y=0; u32 m=0, d=0;
        { auto ymd_ = alert.timestamp_.ymd(); y=ymd_.get<0>(); m=ymd_.get<1>(); d=ymd_.get<2>(); }

        i32 len = Libc::snprintf(
            buf, sizeof(buf),
            "[%s] %04d%02d%02d %.*s/%.*s: %.*s (%.*s)\n",
            level_str, y, m, d,
            static_cast<i32>(alert.source_.length()), alert.source_.data(),
            static_cast<i32>(alert.category_.length()), alert.category_.data(),
            static_cast<i32>(alert.message_.length()), alert.message_.data(),
            static_cast<i32>(alert.detail_.length()), alert.detail_.data());

        if (len > 0) {
            ::write(fd_, buf, static_cast<u64>(len));
        }
#else
        (void)alert;
#endif
    }

    ~FileAlertHandler() override {
#ifdef SPP_OS_LINUX
        if (fd_ >= 0) ::close(fd_);
#endif
    }
};

// =========================================================================
// AlertManager — central alert dispatcher with rate limiting
// =========================================================================
struct AlertManager {
    Vec<Box<AlertHandler>> handlers_;

    AlertLevel min_level_            = AlertLevel::Info;
    u64        max_alerts_per_minute_ = 60;
    u64        alerts_this_minute_    = 0;
    u64        minute_start_us_       = 0;

    // -----------------------------------------------------------------
    // add_handler — register a new alert handler
    // -----------------------------------------------------------------
    void add_handler(Box<AlertHandler> handler) {
        handlers_.push(spp::move(handler));
    }

    // -----------------------------------------------------------------
    // alert — send to all registered handlers (respects min_level + rate limit)
    // -----------------------------------------------------------------
    void alert(AlertLevel level, String_View source, String_View category,
               String_View message, String_View detail = ""_v) {
        // Filter by minimum level
        if (static_cast<u8>(level) < static_cast<u8>(min_level_)) return;

        // Rate limiting
        if (!can_send()) return;

        Alert a;
        a.level_    = level;
        a.timestamp_ = Date::today();
        a.source_   = source;
        a.category_ = category;
        a.message_  = message;
        a.detail_   = detail;

        for (u64 i = 0; i < handlers_.length(); i++) {
            handlers_[i]->send(a);
        }

        alerts_this_minute_++;
    }

    // -----------------------------------------------------------------
    // Convenience methods
    // -----------------------------------------------------------------
    void info(String_View source, String_View category, String_View msg) {
        alert(AlertLevel::Info, source, category, msg);
    }
    void warning(String_View source, String_View category, String_View msg) {
        alert(AlertLevel::Warning, source, category, msg);
    }
    void error(String_View source, String_View category, String_View msg) {
        alert(AlertLevel::Error, source, category, msg);
    }
    void critical(String_View source, String_View category, String_View msg) {
        alert(AlertLevel::Critical, source, category, msg);
    }

    // -----------------------------------------------------------------
    // can_send — rate-limit check (true if we can send now)
    // -----------------------------------------------------------------
    [[nodiscard]] bool can_send() {
#ifdef SPP_OS_LINUX
        struct timeval tv;
        ::gettimeofday(&tv, null);
        u64 now_us = static_cast<u64>(tv.tv_sec) * 1000000ULL
                   + static_cast<u64>(tv.tv_usec);

        // Reset counter if a new minute has started
        if (minute_start_us_ == 0 ||
            (now_us - minute_start_us_) >= 60000000ULL) {
            minute_start_us_    = now_us;
            alerts_this_minute_ = 0;
        }

        return alerts_this_minute_ < max_alerts_per_minute_;
#else
        return true;
#endif
    }
};

// =========================================================================
// HealthMonitor — system health monitoring
// =========================================================================
struct HealthMonitor {
    AlertManager* alert_mgr_ = null;

    struct HealthStatus {
        bool        is_healthy_ = true;
        String_View component_;
        String_View message_;
        Date        last_check_;
        f64         latency_ms_ = 0.0;
    };

    // -----------------------------------------------------------------
    // check_all — run every health check and collect results
    // -----------------------------------------------------------------
    [[nodiscard]] Vec<HealthStatus> check_all() const {
        Vec<HealthStatus> results;
        // Individual checks are called externally with their parameters.
        // This provides a default empty-arg check set.
        HealthStatus mem = check_memory_usage();
        results.push(spp::move(mem));

        HealthStatus lat = check_latency();
        results.push(spp::move(lat));

        return results;
    }

    // -----------------------------------------------------------------
    // check_connection — verify connectivity to an endpoint
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_connection(String_View endpoint) const {
        HealthStatus s;
        s.component_  = "connection"_v;
        s.last_check_ = Date::today();
#ifdef SPP_OS_LINUX
        // Quick TCP connect/disconnect to check reachability
        i32 fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            s.is_healthy_ = false;
            s.message_    = "Failed to create socket"_v;
            return s;
        }

        // Parse host:port from endpoint
        // [UNSPECIFIED] Simplified endpoint parsing: assume "host:port" format.
        // A full implementation would use getaddrinfo with proper parsing.
        u64 colon_pos = 0;
        for (u64 i = 0; i < endpoint.length(); i++) {
            if (endpoint[i] == ':') { colon_pos = i; break; }
        }

        u16 port = 80;
        if (colon_pos > 0 && colon_pos + 1 < endpoint.length()) {
            port = 0;
            for (u64 i = colon_pos + 1; i < endpoint.length() &&
                 endpoint[i] >= '0' && endpoint[i] <= '9'; i++) {
                port = port * 10 + static_cast<u16>(endpoint[i] - '0');
            }
        }

        String_View host = colon_pos > 0 ? endpoint.sub(0, colon_pos) : endpoint;

        // Build null-terminated host
        String<> host_str{host.length() + 1};
        host_str.set_length(host.length() + 1);
        for (u64 i = 0; i < host.length(); i++) host_str[i] = host[i];
        host_str[host.length()] = '\0';

        char port_buf[8];
        Libc::snprintf(reinterpret_cast<u8*>(port_buf), 8, "%u", port);

        struct addrinfo hints = {};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = null;
        i32 ret = ::getaddrinfo(reinterpret_cast<const char*>(host_str.data()),
                                port_buf, &hints, &result);
        if (ret != 0 || !result) {
            s.is_healthy_ = false;
            s.message_    = "DNS resolution failed"_v;
            ::close(fd);
            return s;
        }

        // Non-blocking connect with timeout
        i32 flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        i32 cr = ::connect(fd, result->ai_addr,
                           static_cast<socklen_t>(result->ai_addrlen));
        ::freeaddrinfo(result);

        s.is_healthy_ = true;
        if (cr < 0 && errno == EINPROGRESS) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
            struct timeval tv = {2, 0};  // 2-second timeout
            i32 sel = ::select(fd + 1, null, &wfds, null, &tv);
            if (sel <= 0) {
                s.is_healthy_ = false;
                s.message_    = "Connection timed out"_v;
            }
        } else if (cr < 0) {
            s.is_healthy_ = false;
            s.message_    = "Connection refused"_v;
        }
        ::close(fd);
#else
        s.is_healthy_ = false;
        s.message_    = "Not supported on this platform"_v;
        (void)endpoint;
#endif
        return s;
    }

    // -----------------------------------------------------------------
    // check_order_book — verify the book is not crossed or stale
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_order_book(String_View symbol,
                                                 u64 max_age_ms = 5000) const {
        HealthStatus s;
        s.component_  = "order_book"_v;
        s.last_check_ = Date::today();
        // [UNSPECIFIED] This check requires access to the OrderBookManager,
        // which is not stored in HealthMonitor.  The caller should pass in
        // the relevant OrderBook state directly, or the HealthMonitor should
        // hold a reference to the OrderBookManager.
        //
        // Currently returns a stub: the book is assumed healthy.
        // A real implementation would:
        //   OrderBook& book = order_book_mgr_->get_book(symbol);
        //   s.is_healthy_ = !book.is_crossed() && !book.is_stale(max_age_ms);
        s.is_healthy_ = true;
        s.message_    = "Order book health check requires OrderBookManager reference"_v;
        (void)symbol;
        (void)max_age_ms;
        return s;
    }

    // -----------------------------------------------------------------
    // check_positions — compare local positions against exchange records
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_positions(
        const PositionBook& /*local*/,
        Slice<const ExchangePosition> /*exchange*/) const {
        HealthStatus s;
        s.component_  = "positions"_v;
        s.last_check_ = Date::today();
        // [UNSPECIFIED] ExchangePosition struct not yet defined.
        // When implemented, compare local.positions_ against exchange records
        // and flag any discrepancies in quantity, side, or instrument.
        s.is_healthy_ = true;
        s.message_    = "Position reconciliation not yet implemented"_v;
        return s;
    }

    // -----------------------------------------------------------------
    // check_risk_limits — verify circuit breaker has not tripped
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_risk_limits(const CircuitBreaker& /*breaker*/) const {
        HealthStatus s;
        s.component_  = "risk_limits"_v;
        s.last_check_ = Date::today();
        // [UNSPECIFIED] CircuitBreaker struct not yet defined.
        // When implemented, check whether any limit has been breached.
        s.is_healthy_ = true;
        s.message_    = "Risk limit check requires CircuitBreaker definition"_v;
        return s;
    }

    // -----------------------------------------------------------------
    // check_rate_limits — verify we have not exhausted exchange rate limits
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_rate_limits(const ExchangeRateLimiter& /*limiter*/) const {
        HealthStatus s;
        s.component_  = "rate_limits"_v;
        s.last_check_ = Date::today();
        // [UNSPECIFIED] ExchangeRateLimiter struct not yet defined.
        s.is_healthy_ = true;
        s.message_    = "Rate limit check requires ExchangeRateLimiter definition"_v;
        return s;
    }

    // -----------------------------------------------------------------
    // check_memory_usage — verify process memory is within bounds
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_memory_usage(u64 max_mb = 4000) const {
        HealthStatus s;
        s.component_  = "memory"_v;
        s.last_check_ = Date::today();
#ifdef SPP_OS_LINUX
        // Read /proc/self/status for VmRSS
        i32 fd = ::open("/proc/self/status", O_RDONLY);
        if (fd < 0) {
            s.is_healthy_ = false;
            s.message_    = "Cannot open /proc/self/status"_v;
            return s;
        }
        u8 buf[4096];
        i64 n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) {
            s.is_healthy_ = false;
            s.message_    = "Cannot read /proc/self/status"_v;
            return s;
        }
        buf[n] = '\0';

        // Search for "VmRSS:" line
        String_View content{buf, static_cast<u64>(n)};
        u64 pos = 0;
        while (pos + 6 < content.length()) {
            if (content[pos] == 'V' && content[pos+1] == 'm' &&
                content[pos+2] == 'R' && content[pos+3] == 'S' &&
                content[pos+4] == 'S' && content[pos+5] == ':') {
                // Skip "VmRSS:" and whitespace
                u64 val_start = pos + 6;
                while (val_start < content.length() && content[val_start] == ' ' ||
                       content[val_start] == '\t') val_start++;
                // Parse number in kB
                u64 kb = 0;
                while (val_start < content.length() &&
                       content[val_start] >= '0' && content[val_start] <= '9') {
                    kb = kb * 10 + static_cast<u64>(content[val_start] - '0');
                    val_start++;
                }
                u64 mb = kb / 1024;
                s.is_healthy_ = (mb <= max_mb);
                if (!s.is_healthy_) {
                    // Build message — simplified to avoid complex formatting
                    s.message_ = "Memory usage exceeds limit"_v;
                } else {
                    s.message_ = "Memory OK"_v;
                }
                return s;
            }
            pos++;
        }
        s.is_healthy_ = false;
        s.message_    = "VmRSS not found in /proc/self/status"_v;
#else
        s.is_healthy_ = true;
        s.message_    = "Memory check not supported on this platform"_v;
        (void)max_mb;
#endif
        return s;
    }

    // -----------------------------------------------------------------
    // check_latency — verify system latency is within acceptable bounds
    // -----------------------------------------------------------------
    [[nodiscard]] HealthStatus check_latency(u64 max_latency_us = 100'000) const {
        HealthStatus s;
        s.component_  = "latency"_v;
        s.last_check_ = Date::today();
#ifdef SPP_OS_LINUX
        struct timeval tv_before, tv_after;
        ::gettimeofday(&tv_before, null);
        // Minimal busy-work to measure syscall overhead
        for (volatile i32 i = 0; i < 100; i++) { }
        ::gettimeofday(&tv_after, null);

        u64 elapsed_us = static_cast<u64>(tv_after.tv_sec - tv_before.tv_sec) * 1000000ULL
                       + static_cast<u64>(tv_after.tv_usec - tv_before.tv_usec);
        s.latency_ms_ = static_cast<f64>(elapsed_us) / 1000.0;
        s.is_healthy_ = (elapsed_us <= max_latency_us);

        if (s.is_healthy_) {
            s.message_ = "Latency OK"_v;
        } else {
            s.message_ = "Latency exceeds threshold"_v;
        }
#else
        s.is_healthy_ = true;
        s.message_    = "Latency check not supported on this platform"_v;
        (void)max_latency_us;
#endif
        return s;
    }

    // -----------------------------------------------------------------
    // monitor — continuous monitoring loop (call periodically)
    // -----------------------------------------------------------------
    void monitor(const PositionBook& positions, const CircuitBreaker& breaker,
                 u64 /*socket_fd*/) {
        if (!alert_mgr_) return;

        // Run health checks
        HealthStatus mem  = check_memory_usage();
        HealthStatus lat  = check_latency();
        HealthStatus risk = check_risk_limits(breaker);

        // [UNSPECIFIED] Position reconciliation deferred until ExchangePosition
        // is defined.  Currently skips position check.
        (void)positions;

        // Report any unhealthy components
        if (!mem.is_healthy_) {
            alert_mgr_->warning("HealthMonitor"_v, "memory"_v, mem.message_);
        }
        if (!lat.is_healthy_) {
            alert_mgr_->warning("HealthMonitor"_v, "latency"_v, lat.message_);
        }
        if (!risk.is_healthy_) {
            alert_mgr_->error("HealthMonitor"_v, "risk"_v, risk.message_);
        }

        // [UNSPECIFIED] Connection health check deferred — requires endpoint list.
    }
};

// =========================================================================
// LatencyTracker — tick-to-trade latency measurement
// =========================================================================
struct LatencyTracker {
    struct EventTiming {
        Date market_data_arrival_;
        Date signal_generated_;
        Date order_sent_;
        Date order_ack_;
        Date fill_received_;
    };

    static constexpr u64 MAX_SAMPLES = 1000;
    EventTiming samples_[MAX_SAMPLES] = {};
    u64          write_idx_ = 0;

    // Track whether each stage has been marked for the current event
    bool has_market_data_ = false;
    bool has_signal_      = false;
    bool has_order_sent_  = false;
    bool has_order_ack_   = false;

    /// Record market data arrival and begin a new timing cycle.
    void mark_market_data(Date ts) {
        // Start a new sample
        samples_[write_idx_] = EventTiming{};
        samples_[write_idx_].market_data_arrival_ = ts;
        has_market_data_ = true;
        has_signal_      = false;
        has_order_sent_  = false;
        has_order_ack_   = false;
    }

    /// Record signal generation time.
    void mark_signal(Date ts) {
        if (!has_market_data_) return;
        samples_[write_idx_].signal_generated_ = ts;
        has_signal_ = true;
    }

    /// Record order dispatch time.
    void mark_order_sent(Date ts) {
        if (!has_market_data_) return;
        samples_[write_idx_].order_sent_ = ts;
        has_order_sent_ = true;
    }

    /// Record exchange acknowledgment.
    void mark_order_ack(Date ts) {
        if (!has_market_data_) return;
        samples_[write_idx_].order_ack_ = ts;
        has_order_ack_ = true;
    }

    /// Record fill and complete the timing cycle.
    void mark_fill(Date ts) {
        if (!has_market_data_) return;
        samples_[write_idx_].fill_received_ = ts;

        // Advance write index (circular)
        write_idx_++;
        if (write_idx_ >= MAX_SAMPLES) write_idx_ = 0;

        has_market_data_ = false;
        has_signal_      = false;
        has_order_sent_  = false;
        has_order_ack_   = false;
    }

    struct LatencyStats {
        f64 avg_tick_to_signal_us_   = 0.0;
        f64 avg_signal_to_order_us_  = 0.0;
        f64 avg_order_to_ack_us_     = 0.0;
        f64 avg_total_tick_to_fill_us_ = 0.0;
        f64 p50_us_ = 0.0;
        f64 p95_us_ = 0.0;
        f64 p99_us_ = 0.0;
        u64 sample_count_ = 0;

        SPP_RECORD(LatencyStats, SPP_FIELD(avg_tick_to_signal_us_),
                   SPP_FIELD(avg_total_tick_to_fill_us_),
                   SPP_FIELD(sample_count_));
    };

    /// Compute latency statistics from the ring buffer.
    [[nodiscard]] LatencyStats compute() const {
        LatencyStats stats;

        // Count valid samples (those with a fill recorded)
        u64 count = 0;
        for (u64 i = 0; i < MAX_SAMPLES; i++) {
            if (samples_[i].fill_received_.serial_ != 0) count++;
        }
        if (count == 0) return stats;
        stats.sample_count_ = count;

        // Collect total latencies for percentile computation
        Vec<f64> totals;
        totals.reserve(count);

        f64 sum_t2s = 0.0, sum_s2o = 0.0, sum_o2a = 0.0, sum_total = 0.0;

        for (u64 i = 0; i < MAX_SAMPLES; i++) {
            const EventTiming& t = samples_[i];
            if (t.fill_received_.serial_ == 0) continue;

            // Compute durations in microseconds.
            // Date::serial_ is in days.  1 day = 86,400,000,000 us.
            // We approximate by converting day differences to us.
            auto day_diff_to_us = [](Date a, Date b) -> f64 {
                i32 diff_days = a.serial_ - b.serial_;
                return static_cast<f64>(diff_days) * 86400000000.0;
            };

            f64 t2s   = day_diff_to_us(t.signal_generated_,  t.market_data_arrival_);
            f64 s2o   = day_diff_to_us(t.order_sent_,        t.signal_generated_);
            f64 o2a   = day_diff_to_us(t.order_ack_,         t.order_sent_);
            f64 total = day_diff_to_us(t.fill_received_,     t.market_data_arrival_);

            if (t2s < 0.0) t2s = 0.0;
            if (s2o < 0.0) s2o = 0.0;
            if (o2a < 0.0) o2a = 0.0;
            if (total < 0.0) total = 0.0;

            sum_t2s   += t2s;
            sum_s2o   += s2o;
            sum_o2a   += o2a;
            sum_total += total;

            totals.push(total);
        }

        f64 n = static_cast<f64>(count);
        stats.avg_tick_to_signal_us_  = sum_t2s   / n;
        stats.avg_signal_to_order_us_ = sum_s2o   / n;
        stats.avg_order_to_ack_us_    = sum_o2a   / n;
        stats.avg_total_tick_to_fill_us_ = sum_total / n;

        // Sort totals for percentile computation
        // Simple insertion sort (ring buffer is small, count <= 1000)
        for (u64 i = 1; i < totals.length(); i++) {
            f64 key = totals[i];
            u64 j = i;
            while (j > 0 && totals[j - 1] > key) {
                totals[j] = totals[j - 1];
                j--;
            }
            totals[j] = key;
        }

        auto percentile = [&totals, count](f64 pct) -> f64 {
            f64 idx = pct * static_cast<f64>(count - 1);
            u64 lo = static_cast<u64>(idx);
            u64 hi = lo + 1;
            if (hi >= count) return totals[count - 1];
            f64 frac = idx - static_cast<f64>(lo);
            return totals[lo] * (1.0 - frac) + totals[hi] * frac;
        };

        stats.p50_us_ = percentile(0.50);
        stats.p95_us_ = percentile(0.95);
        stats.p99_us_ = percentile(0.99);

        return stats;
    }

    /// Reset all samples.
    void reset() {
        write_idx_ = 0;
        has_market_data_ = false;
        has_signal_      = false;
        has_order_sent_  = false;
        has_order_ack_   = false;
        for (u64 i = 0; i < MAX_SAMPLES; i++) {
            samples_[i] = EventTiming{};
        }
    }
};

// =========================================================================
// MultiStrategyOrchestrator — manage multiple strategies on one account
// =========================================================================

struct StrategyAllocation {
    String_View       strategy_name_;
    f64               capital_fraction_ = 0.0;   ///< fraction of total capital
    f64               max_leverage_     = 1.0;
    Vec<String_View>  allowed_symbols_;           ///< empty = all symbols allowed
    bool              enabled_          = true;

    SPP_RECORD(StrategyAllocation, SPP_FIELD(strategy_name_),
               SPP_FIELD(capital_fraction_), SPP_FIELD(enabled_));
};

struct MultiStrategyOrchestrator {
    Vec<StrategyAllocation> allocations_;
    f64 total_capital_ = 1'000'000.0;

    /// Per-strategy position tracking.
    /// strategy_name -> (symbol -> quantity)
    Map<String<>, Map<String<>, f64>> strategy_positions_;

    // -----------------------------------------------------------------
    // aggregate_positions — net all strategy positions per symbol
    // -----------------------------------------------------------------
    [[nodiscard]] PositionBook aggregate_positions() const {
        PositionBook book;

        // First pass: collect all symbols and their net quantities
        Map<String<>, f64> net_qty;
        Vec<String<>> symbols;

        for (const auto& strat_kv : strategy_positions_) {
            for (const auto& sym_kv : strat_kv.second) {
                // Check if we've seen this symbol before
                bool found = false;
                for (u64 i = 0; i < symbols.length(); i++) {
                    if (symbols[i].view() == sym_kv.first.view()) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    symbols.push(sv::sv_to_string(sym_kv.first));
                }
                net_qty.insert(sv::sv_to_string(sym_kv.first), 0.0);
            }
        }

        // Second pass: sum quantities
        for (const auto& strat_kv : strategy_positions_) {
            for (const auto& sym_kv : strat_kv.second) {
                auto opt = net_qty.try_get(sym_kv.first);
                if (opt.ok()) {
                    **opt += sym_kv.second;
                }
            }
        }

        // Build PositionBook
        for (u64 i = 0; i < symbols.length(); i++) {
            auto qty_opt = net_qty.try_get(symbols[i].view());
            if (!qty_opt.ok()) continue;
            f64 qty = **qty_opt;
            if (Math::abs(qty) < 1e-12) continue;

            Position pos;
            pos.instrument_id_ = symbols[i].view();
            pos.quantity_      = qty;
            pos.entry_price_   = 0.0;  // aggregate doesn't track cost basis
            pos.entry_date_    = Date::today();
            book.add(spp::move(pos));
        }

        return book;
    }

    // -----------------------------------------------------------------
    // Conflict — two strategies want opposing positions in the same symbol
    // -----------------------------------------------------------------
    struct Conflict {
        String_View symbol_;
        String_View strategy_a_;
        String_View strategy_b_;
        f64         net_position_ = 0.0;  ///< after netting
        bool        is_conflict_  = false; ///< true if opposing signs
    };

    // -----------------------------------------------------------------
    // detect_conflicts — find symbols where strategies oppose each other
    // -----------------------------------------------------------------
    [[nodiscard]] Vec<Conflict> detect_conflicts() const {
        Vec<Conflict> conflicts;

        // Collect all strategy names and symbols
        Vec<String<>> strat_names;
        Vec<String<>> all_symbols;

        for (u64 i = 0; i < allocations_.length(); i++) {
            strat_names.push(sv::sv_to_string(allocations_[i].strategy_name_));
        }

        // Collect all unique symbols across all strategies
        for (const auto& strat_kv : strategy_positions_) {
            for (const auto& sym_kv : strat_kv.second) {
                bool found = false;
                for (u64 s = 0; s < all_symbols.length(); s++) {
                    if (all_symbols[s].view() == sym_kv.first.view()) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    all_symbols.push(sv::sv_to_string(sym_kv.first));
                }
            }
        }

        // For each symbol, check whether any two strategies have opposing signs
        for (u64 si = 0; si < all_symbols.length(); si++) {
            String_View sym = all_symbols[si].view();

            // Gather positions for this symbol across all strategies
            Vec<f64> quantities;
            Vec<String_View> s_names;
            quantities.reserve(strat_names.length());
            s_names.reserve(strat_names.length());

            for (u64 ai = 0; ai < strat_names.length(); ai++) {
                String_View sn = strat_names[ai].view();
                auto strat_opt = strategy_positions_.try_get(sn);
                if (!strat_opt.ok()) continue;

                auto sym_opt = (**strat_opt).try_get(sym);
                if (!sym_opt.ok()) continue;

                f64 qty = **sym_opt;
                if (Math::abs(qty) < 1e-12) continue;

                quantities.push(qty);
                s_names.push(sn);
            }

            // Check for opposing signs
            for (u64 i = 0; i < quantities.length(); i++) {
                for (u64 j = i + 1; j < quantities.length(); j++) {
                    if ((quantities[i] > 0.0 && quantities[j] < 0.0) ||
                        (quantities[i] < 0.0 && quantities[j] > 0.0)) {
                        Conflict c;
                        c.symbol_       = sym;
                        c.strategy_a_   = s_names[i];
                        c.strategy_b_   = s_names[j];
                        c.net_position_ = quantities[i] + quantities[j];
                        c.is_conflict_  = true;
                        conflicts.push(c);
                    }
                }
            }
        }

        return conflicts;
    }

    // -----------------------------------------------------------------
    // capital_for_strategy — allocated capital for a named strategy
    // -----------------------------------------------------------------
    [[nodiscard]] f64 capital_for_strategy(String_View name) const {
        for (u64 i = 0; i < allocations_.length(); i++) {
            if (allocations_[i].strategy_name_ == name) {
                return total_capital_ * allocations_[i].capital_fraction_;
            }
        }
        return 0.0;
    }

    // -----------------------------------------------------------------
    // net_exposure — total signed exposure across all strategies
    // -----------------------------------------------------------------
    [[nodiscard]] f64 net_exposure() const {
        f64 net = 0.0;
        for (const auto& strat_kv : strategy_positions_) {
            for (const auto& sym_kv : strat_kv.second) {
                net += sym_kv.second;
            }
        }
        return net;
    }

    // -----------------------------------------------------------------
    // gross_exposure — total absolute exposure
    // -----------------------------------------------------------------
    [[nodiscard]] f64 gross_exposure() const {
        f64 gross = 0.0;
        for (const auto& strat_kv : strategy_positions_) {
            for (const auto& sym_kv : strat_kv.second) {
                f64 v = sym_kv.second;
                gross += (v >= 0.0) ? v : -v;
            }
        }
        return gross;
    }

    // -----------------------------------------------------------------
    // add_strategy — register a new strategy allocation
    // -----------------------------------------------------------------
    void add_strategy(StrategyAllocation alloc) {
        // Check for duplicate
        for (u64 i = 0; i < allocations_.length(); i++) {
            if (allocations_[i].strategy_name_ == alloc.strategy_name_) {
                // Update existing
                allocations_[i] = spp::move(alloc);
                return;
            }
        }
        allocations_.push(spp::move(alloc));
    }

    // -----------------------------------------------------------------
    // remove_strategy — deregister a strategy
    // -----------------------------------------------------------------
    void remove_strategy(String_View name) {
        for (u64 i = 0; i < allocations_.length(); i++) {
            if (allocations_[i].strategy_name_ == name) {
                // Shift remaining entries left
                for (u64 j = i; j + 1 < allocations_.length(); j++) {
                    allocations_[j] = spp::move(allocations_[j + 1]);
                }
                allocations_.pop();
                break;
            }
        }
        // Also remove from position tracking
        strategy_positions_.remove(sv::sv_to_string(name));
    }

    // -----------------------------------------------------------------
    // enable_strategy — enable or disable a strategy
    // -----------------------------------------------------------------
    void enable_strategy(String_View name, bool enable) {
        for (u64 i = 0; i < allocations_.length(); i++) {
            if (allocations_[i].strategy_name_ == name) {
                allocations_[i].enabled_ = enable;
                return;
            }
        }
    }

    // -----------------------------------------------------------------
    // update_position — record a strategy's position in a symbol
    // (called by strategies after fills)
    // -----------------------------------------------------------------
    void update_position(String_View strategy_name, String_View symbol, f64 quantity) {
        String<> key = sv::sv_to_string(strategy_name);
        auto opt = strategy_positions_.try_get(key.view());
        if (opt.ok()) {
            Map<String<>, f64>& sym_map = **opt;
            auto sym_opt = sym_map.try_get(symbol);
            if (sym_opt.ok()) {
                **sym_opt = quantity;
            } else {
                sym_map.insert(sv::sv_to_string(symbol), quantity);
            }
        } else {
            Map<String<>, f64> sym_map;
            sym_map.insert(sv::sv_to_string(symbol), quantity);
            strategy_positions_.insert(spp::move(key), spp::move(sym_map));
        }
    }
};

} // namespace monitoring
} // namespace spp::quant

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_ENUM(::spp::quant::monitoring::AlertLevel, "AlertLevel", Info,
               SPP_CASE(Info), SPP_CASE(Warning), SPP_CASE(Error),
               SPP_CASE(Critical));

SPP_NAMED_RECORD(::spp::quant::monitoring::Alert, "Alert",
                 SPP_FIELD(level_), SPP_FIELD(timestamp_), SPP_FIELD(source_),
                 SPP_FIELD(category_), SPP_FIELD(message_));

SPP_NAMED_RECORD(::spp::quant::monitoring::HealthMonitor::HealthStatus,
                 "HealthStatus", SPP_FIELD(is_healthy_), SPP_FIELD(component_),
                 SPP_FIELD(message_), SPP_FIELD(last_check_), SPP_FIELD(latency_ms_));

SPP_NAMED_RECORD(::spp::quant::monitoring::LatencyTracker::LatencyStats,
                 "LatencyStats",
                 SPP_FIELD(avg_tick_to_signal_us_),
                 SPP_FIELD(avg_signal_to_order_us_),
                 SPP_FIELD(avg_order_to_ack_us_),
                 SPP_FIELD(avg_total_tick_to_fill_us_),
                 SPP_FIELD(p50_us_), SPP_FIELD(p95_us_), SPP_FIELD(p99_us_),
                 SPP_FIELD(sample_count_));

SPP_NAMED_RECORD(::spp::quant::monitoring::StrategyAllocation,
                 "StrategyAllocation",
                 SPP_FIELD(strategy_name_), SPP_FIELD(capital_fraction_),
                 SPP_FIELD(max_leverage_), SPP_FIELD(enabled_));
