#pragma once

#include <spp/core/base.h>
#include <spp/protocol/http.h>

namespace spp::App::Binance {

// Rate-limit accounting + backoff for Binance Spot REST.
//
// Inputs:
//   - request_weight per endpoint (set by the caller — Binance docs list it)
//   - response headers X-MBX-USED-WEIGHT-<INTERVAL> from each response
//   - HTTP 429 (with optional Retry-After) for soft throttling
//   - HTTP 418 for hard IP ban
//
// Outputs:
//   - wait_required_ms(weight, now_ms): how long the caller must sleep before
//     the next request is permitted. 0 = OK now.
//   - update_from(response, now_ms): refresh `used` counters from headers and
//     install a cooldown on 429/418.
//
// The class does not call gettimeofday itself — callers pass `now_ms` so unit
// tests can be deterministic.

struct Rate_Limiter {
    struct Bucket {
        String_View interval; // "1M" / "1H" / etc., echoed back from server
        i64 limit = 0;
        i64 used = 0;
        i64 window_ms = 0;
        i64 window_started_at_ms = 0;
    };

    // Conservative default for Binance Spot: 1200 weight per minute.
    explicit Rate_Limiter(i64 spot_1m_limit = 1200) noexcept {
        buckets.push(Bucket{"1M"_v, spot_1m_limit, 0, 60000, 0});
    }

    Vec<Bucket, Mdefault> buckets;

    // 0 = no active cooldown; >0 = abs ms-since-epoch when sending is allowed.
    i64 cooldown_until_ms = 0;
    bool ip_banned = false;

    // Pre-flight: how many ms the caller must wait before sending a request
    // with this weight. Returns 0 if the request fits the current budget.
    [[nodiscard]] i64 wait_required_ms(i64 weight, i64 now_ms) noexcept {
        if(now_ms < cooldown_until_ms) return cooldown_until_ms - now_ms;
        for(auto& b : buckets) {
            // Roll over expired window.
            if(b.window_started_at_ms == 0) {
                b.window_started_at_ms = now_ms;
            } else if(now_ms - b.window_started_at_ms >= b.window_ms) {
                b.used = 0;
                b.window_started_at_ms = now_ms;
            }
            if(b.used + weight > b.limit) {
                return b.window_started_at_ms + b.window_ms - now_ms;
            }
        }
        return 0;
    }

    // Caller invokes this AFTER any request, regardless of HTTP status, to
    // refresh the used-weight counters and install any necessary cooldown.
    template<Allocator A>
    void update_from(const Protocol::Http::Response<A>& resp, i64 weight, i64 now_ms) noexcept {
        // 1. Refresh `used` from X-MBX-USED-WEIGHT-* headers when present.
        bool found_specific = false;
        for(auto& b : buckets) {
            for(const auto& h : resp.headers) {
                if(header_matches_used_weight_(h.name, b.interval)) {
                    auto v = parse_decimal_(h.value);
                    if(v.ok()) {
                        b.used = v.unwrap();
                        if(b.window_started_at_ms == 0) b.window_started_at_ms = now_ms;
                        found_specific = true;
                    }
                }
            }
        }
        // Fallback: best-effort local increment if the server didn't echo a
        // per-interval header (older endpoints / preflight 4xx with no rate
        // headers).
        if(!found_specific && weight > 0) {
            for(auto& b : buckets) {
                if(b.window_started_at_ms == 0) b.window_started_at_ms = now_ms;
                b.used += weight;
            }
        }

        // 2. Install cooldown on 429 / 418.
        if(resp.status_code == 429 || resp.status_code == 418) {
            i64 retry_after_ms = 0;
            auto h = resp.find_header("Retry-After"_v);
            if(h.ok()) {
                auto parsed = parse_decimal_(*h);
                if(parsed.ok()) retry_after_ms = parsed.unwrap() * 1000;
            }
            if(retry_after_ms == 0) {
                // No header — pick a reasonable floor. 418 = IP ban, much
                // longer than transient 429.
                retry_after_ms = resp.status_code == 418 ? 120000 : 5000;
            }
            cooldown_until_ms = now_ms + retry_after_ms;
            if(resp.status_code == 418) ip_banned = true;
        }
    }

    // Marks the limiter healthy again — call after a successful 2xx.
    void clear_cooldown_if_passed(i64 now_ms) noexcept {
        if(now_ms >= cooldown_until_ms) {
            cooldown_until_ms = 0;
            ip_banned = false;
        }
    }

private:
    [[nodiscard]] static bool header_matches_used_weight_(String_View name,
                                                          String_View interval) noexcept {
        // Match X-MBX-USED-WEIGHT-<INTERVAL>.
        static const String_View prefix = "X-MBX-USED-WEIGHT-"_v;
        if(name.length() <= prefix.length()) return false;
        if(!ieq_(name.sub(0, prefix.length()), prefix)) return false;
        return ieq_(name.sub(prefix.length(), name.length()), interval);
    }

    [[nodiscard]] static bool ieq_(String_View a, String_View b) noexcept {
        if(a.length() != b.length()) return false;
        for(u64 i = 0; i < a.length(); i++) {
            u8 x = a[i];
            u8 y = b[i];
            if(x >= 'a' && x <= 'z') x = x - 32;
            if(y >= 'a' && y <= 'z') y = y - 32;
            if(x != y) return false;
        }
        return true;
    }

    [[nodiscard]] static Result<i64, String_View> parse_decimal_(String_View s) noexcept {
        if(s.empty()) return Result<i64, String_View>::err("empty"_v);
        i64 v = 0;
        u64 i = 0;
        while(i < s.length() && (s[i] == ' ' || s[i] == '\t')) i++;
        bool neg = false;
        if(i < s.length() && (s[i] == '+' || s[i] == '-')) {
            neg = s[i] == '-';
            i++;
        }
        if(i == s.length()) return Result<i64, String_View>::err("empty"_v);
        for(; i < s.length(); i++) {
            u8 c = s[i];
            if(c < '0' || c > '9') break;
            v = v * 10 + (c - '0');
        }
        return Result<i64, String_View>::ok(spp::move(neg ? -v : v));
    }
};

} // namespace spp::App::Binance
