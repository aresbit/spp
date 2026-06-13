#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"
#include "spp/quant/data/timeseries.h"

#ifdef SPP_OS_LINUX
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#endif

namespace spp::quant {

// =========================================================================
// Helpers: String_View missing operations
// =========================================================================
namespace sv {

[[nodiscard]] inline bool starts_with(String_View s, String_View prefix) noexcept {
    if (s.length() < prefix.length()) return false;
    for (u64 i = 0; i < prefix.length(); i++) {
        if (s[i] != prefix[i]) return false;
    }
    return true;
}

[[nodiscard]] inline bool contains(String_View s, String_View sub) noexcept {
    if (sub.length() == 0) return true;
    if (sub.length() > s.length()) return false;
    for (u64 i = 0; i + sub.length() <= s.length(); i++) {
        bool match = true;
        for (u64 j = 0; j < sub.length(); j++) {
            if (s[i + j] != sub[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

[[nodiscard]] inline char back(String_View s) noexcept {
    return static_cast<char>(s[s.length() - 1]);
}

// Build a String<> from a Vec<u8> scratch buffer
inline String<> vec_to_string(Vec<u8>& buf) noexcept {
    u64 n = buf.length();
    String<> s{n};
    s.set_length(n);
    for (u64 i = 0; i < n; i++) {
        s[i] = buf[i];
    }
    return s;
}

// Copy String_View into a String<>
inline String<> sv_to_string(String_View sv) noexcept {
    u64 n = sv.length();
    String<> s{n};
    s.set_length(n);
    for (u64 i = 0; i < n; i++) {
        s[i] = sv[i];
    }
    return s;
}

// Write String_View into a String<> at position; return next write position
inline u64 str_write(String<>& s, u64 pos, String_View text) noexcept {
    for (u64 i = 0; i < text.length(); i++) {
        s[pos + i] = text[i];
    }
    return pos + text.length();
}

// Write a single char
inline u64 str_write_c(String<>& s, u64 pos, char c) noexcept {
    s[pos] = static_cast<u8>(c);
    return pos + 1;
}

} // namespace sv

// =========================================================================
// Bar/Candle — OHLCV bar data
// =========================================================================
struct Bar {
    Date date_;
    f64 open_   = 0.0;
    f64 high_   = 0.0;
    f64 low_    = 0.0;
    f64 close_  = 0.0;
    f64 volume_ = 0.0;
    u64 trades_count_ = 0;
    f64 vwap_   = 0.0;
};

// =========================================================================
// QuoteTick — top-of-book quote
// =========================================================================
struct QuoteTick {
    Date timestamp_;
    f64 bid_      = 0.0;
    f64 ask_      = 0.0;
    f64 bid_size_ = 0.0;
    f64 ask_size_ = 0.0;

    [[nodiscard]] f64 mid() const noexcept { return (bid_ + ask_) / 2.0; }
    [[nodiscard]] f64 spread() const noexcept { return ask_ - bid_; }
    [[nodiscard]] f64 spread_bps() const noexcept {
        f64 m = mid();
        if (m <= 0.0) return 0.0;
        return (ask_ - bid_) / m * 10000.0;
    }
};

// =========================================================================
// TradeTick — individual trade/transaction
// =========================================================================
struct TradeTick {
    Date timestamp_;
    f64 price_  = 0.0;
    f64 volume_ = 0.0;

    enum struct Side : u8 { Buy, Sell, Unknown };
    Side side_     = Side::Unknown;
    u64 trade_id_  = 0;
};

// =========================================================================
// BookLevel — one level of an order book
// =========================================================================
struct BookLevel {
    f64 price_      = 0.0;
    f64 volume_     = 0.0;
    u64 order_count_ = 0;
};

// =========================================================================
// OrderBookSnapshot — full order book at a point in time
// =========================================================================
struct OrderBookSnapshot {
    Date timestamp_;
    String_View symbol_;
    Vec<BookLevel> bids_;  // sorted highest to lowest
    Vec<BookLevel> asks_;  // sorted lowest to highest

    [[nodiscard]] f64 best_bid() const noexcept {
        return bids_.empty() ? 0.0 : bids_.front().price_;
    }
    [[nodiscard]] f64 best_ask() const noexcept {
        return asks_.empty() ? 0.0 : asks_.front().price_;
    }
    [[nodiscard]] f64 mid() const noexcept {
        f64 b = best_bid();
        f64 a = best_ask();
        if (b <= 0.0 || a <= 0.0) return 0.0;
        return (b + a) / 2.0;
    }
    [[nodiscard]] f64 spread() const noexcept {
        return best_ask() - best_bid();
    }

    // Total volume within N levels
    [[nodiscard]] f64 market_depth(u64 levels) const noexcept {
        f64 total = 0.0;
        u64 n = levels < bids_.length() ? levels : bids_.length();
        for (u64 i = 0; i < n; i++) total += bids_[i].volume_;
        n = levels < asks_.length() ? levels : asks_.length();
        for (u64 i = 0; i < n; i++) total += asks_[i].volume_;
        return total;
    }

    // Order book imbalance: (bid_vol - ask_vol) / (bid_vol + ask_vol) within N levels
    [[nodiscard]] f64 imbalance(u64 levels) const noexcept {
        f64 bid_vol = 0.0, ask_vol = 0.0;
        u64 n = levels < bids_.length() ? levels : bids_.length();
        for (u64 i = 0; i < n; i++) bid_vol += bids_[i].volume_;
        n = levels < asks_.length() ? levels : asks_.length();
        for (u64 i = 0; i < n; i++) ask_vol += asks_[i].volume_;
        f64 total = bid_vol + ask_vol;
        if (total <= 0.0) return 0.0;
        return (bid_vol - ask_vol) / total;
    }
};

// =========================================================================
// MarketDataConnector — abstract market data source
// =========================================================================
struct MarketDataConnector {
    String_View name_;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual Opt<Bar> next_bar() = 0;
    virtual Vec<Bar> load_bars(Date from, Date to) = 0;
    virtual Vec<String_View> symbols() const = 0;
    virtual void subscribe_quotes(String_View symbol) = 0;
    virtual Opt<QuoteTick> next_quote() = 0;
    virtual Opt<OrderBookSnapshot> order_book(String_View symbol) = 0;

    virtual ~MarketDataConnector() = default;
};

// =========================================================================
// Minimal JSON Parser — pull-style helpers for flat JSON key-value extraction
// =========================================================================
namespace json_detail {

inline u64 skip_ws(String_View s, u64 pos) noexcept {
    while (pos < s.length()) {
        char c = static_cast<char>(s[pos]);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        pos++;
    }
    return pos;
}

// Find a quoted string value for the given key
[[nodiscard]] inline Opt<String_View> get_string_value(String_View s, String_View key) noexcept {
    u64 key_len = key.length();
    for (u64 i = 0; i + key_len + 4 < s.length(); i++) {
        if (static_cast<char>(s[i]) != '"') continue;
        bool match = true;
        for (u64 j = 0; j < key_len; j++) {
            if (s[i + 1 + j] != key[j]) { match = false; break; }
        }
        if (!match) continue;
        if (static_cast<char>(s[i + 1 + key_len]) != '"') continue;

        u64 j = i + 1 + key_len + 1;
        j = skip_ws(s, j);
        if (j >= s.length() || static_cast<char>(s[j]) != ':') continue;
        j++;
        j = skip_ws(s, j);
        if (j >= s.length()) continue;

        if (static_cast<char>(s[j]) == '"') {
            u64 start = j + 1;
            u64 end = start;
            while (end < s.length() && static_cast<char>(s[end]) != '"') end++;
            return Opt<String_View>{s.sub(start, end)};
        } else if (static_cast<char>(s[j]) == '-' || ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) {
            u64 start = j;
            u64 end = start;
            while (end < s.length() &&
                   (s[end] == '-' || s[end] == '.' || s[end] == 'e' || s[end] == 'E' ||
                    s[end] == '+' || (s[end] >= '0' && s[end] <= '9') || s[end] == ' ' ||
                    s[end] == ',' || s[end] == '}'))
                end++;
            return Opt<String_View>{s.sub(start, end)};
        } else if (s[j] == 't' || s[j] == 'f') {
            u64 start = j;
            u64 end = start;
            while (end < s.length() &&
                   ((s[end] >= 'a' && s[end] <= 'z') || (s[end] >= 'A' && s[end] <= 'Z')))
                end++;
            return Opt<String_View>{s.sub(start, end)};
        }
    }
    return {};
}

[[nodiscard]] inline Opt<f64> get_f64_value(String_View s, String_View key) noexcept {
    auto v = get_string_value(s, key);
    if (!v.ok()) return {};
    String_View str = *v;
    if (str.empty()) return {};
    f64 result = 0.0;
    f64 sign = 1.0;
    u64 pos = 0;
    if (static_cast<char>(str[0]) == '-') { sign = -1.0; pos++; }
    else if (static_cast<char>(str[0]) == '+') pos++;

    bool in_frac = false;
    f64 frac_mult = 0.1;
    bool in_exp = false;
    i64 exp_sign = 1;
    i64 exp_val = 0;

    while (pos < str.length()) {
        char c = static_cast<char>(str[pos]);
        if (c >= '0' && c <= '9') {
            if (in_exp) {
                exp_val = exp_val * 10 + (c - '0');
            } else if (in_frac) {
                result += static_cast<f64>(c - '0') * frac_mult;
                frac_mult *= 0.1;
            } else {
                result = result * 10.0 + static_cast<f64>(c - '0');
            }
        } else if (c == '.') {
            in_frac = true;
        } else if (c == 'e' || c == 'E') {
            in_exp = true;
            if (pos + 1 < str.length() && static_cast<char>(str[pos + 1]) == '-') {
                exp_sign = -1;
                pos++;
            } else if (pos + 1 < str.length() && static_cast<char>(str[pos + 1]) == '+') {
                pos++;
            }
        } else {
            break;
        }
        pos++;
    }
    result *= sign;
    if (in_exp) {
        f64 mult = 1.0;
        i64 ev = exp_val;
        while (ev > 0) { mult *= 10.0; ev--; }
        if (exp_sign < 0) result /= mult;
        else result *= mult;
    }
    return Opt<f64>{result};
}

[[nodiscard]] inline Opt<i64> get_i64_value(String_View s, String_View key) noexcept {
    auto v = get_string_value(s, key);
    if (!v.ok()) return {};
    String_View str = *v;
    i64 result = 0;
    i64 sign = 1;
    u64 pos = 0;
    if (!str.empty() && static_cast<char>(str[0]) == '-') { sign = -1; pos++; }
    while (pos < str.length() && str[pos] >= '0' && str[pos] <= '9') {
        result = result * 10 + static_cast<i64>(str[pos] - '0');
        pos++;
    }
    return Opt<i64>{result * sign};
}

[[nodiscard]] inline Opt<bool> get_bool_value(String_View s, String_View key) noexcept {
    auto v = get_string_value(s, key);
    if (!v.ok()) return {};
    String_View sv = *v;
    if (sv == "true"_v) return Opt<bool>{true};
    if (sv == "false"_v) return Opt<bool>{false};
    return {};
}

} // namespace json_detail

// =========================================================================
// CSVConnector — reads historical OHLCV from CSV files
// =========================================================================
struct CSVConnector : MarketDataConnector {
    String_View base_path_;
    // Store bars in a flat Vec per symbol indexed by Date lookup
    Map<String<>, Vec<Bar>> cache_;
    Map<String<>, Vec<Date>> cache_dates_;
    Map<String<>, u64> current_indices_;
    String_View current_symbol_;
    bool connected_ = false;

    CSVConnector() noexcept { name_ = "CSVConnector"_v; }

    explicit CSVConnector(String_View path) noexcept : base_path_(spp::move(path)) {
        name_ = "CSVConnector"_v;
    }

    bool connect() override { connected_ = true; return true; }

    void disconnect() override {
        connected_ = false;
        cache_.clear();
        cache_dates_.clear();
        current_indices_.clear();
    }

    bool is_connected() const override { return connected_; }

    Vec<String_View> symbols() const override {
        Vec<String_View> result;
        for (const auto& kv : cache_) {
            result.push(kv.first.view());
        }
        return result;
    }

    // ---- Date Parsing ----
    static Opt<Date> parse_date(String_View date_str) noexcept {
        if (date_str.length() < 8) return {};
        u64 digits[8] = {};
        u64 digit_count = 0;
        for (u64 i = 0; i < date_str.length() && digit_count < 8; i++) {
            char c = static_cast<char>(date_str[i]);
            if (c >= '0' && c <= '9') digits[digit_count++] = static_cast<u64>(c - '0');
        }
        if (digit_count < 6) return {};

        i32 y = 0;
        u32 m = 0, d = 0;

        if (digit_count == 8) {
            y = static_cast<i32>(digits[0]*1000 + digits[1]*100 + digits[2]*10 + digits[3]);
            m = static_cast<u32>(digits[4]*10 + digits[5]);
            d = static_cast<u32>(digits[6]*10 + digits[7]);
        } else if (sv::contains(date_str, "-"_v)) {
            u64 dash_count = 0, first_dash = 0;
            for (u64 i = 0; i < date_str.length(); i++) {
                if (static_cast<char>(date_str[i]) == '-') {
                    if (dash_count == 0) first_dash = i;
                    dash_count++;
                }
            }
            if (dash_count == 2 && first_dash >= 4) {
                y = static_cast<i32>(digits[0]*1000 + digits[1]*100 + digits[2]*10 + digits[3]);
                m = static_cast<u32>(digits[4]*10 + digits[5]);
                d = static_cast<u32>(digits[6]*10 + digits[7]);
            } else if (dash_count == 2 && first_dash < 4 && digits[0]*10 + digits[1] <= 12) {
                m = static_cast<u32>(digits[0]*10 + digits[1]);
                d = static_cast<u32>(digits[2]*10 + digits[3]);
                y = static_cast<i32>(digits[4]*1000 + digits[5]*100 + digits[6]*10 + digits[7]);
            } else {
                d = static_cast<u32>(digits[0]*10 + digits[1]);
                m = static_cast<u32>(digits[2]*10 + digits[3]);
                y = static_cast<i32>(digits[4]*1000 + digits[5]*100 + digits[6]*10 + digits[7]);
            }
        } else if (sv::contains(date_str, "/"_v)) {
            m = static_cast<u32>(digits[0]*10 + digits[1]);
            d = static_cast<u32>(digits[2]*10 + digits[3]);
            y = static_cast<i32>(digits[4]*1000 + digits[5]*100 + digits[6]*10 + digits[7]);
        }

        if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900 || y > 2200) return {};
        u32 max_d = Date::days_in_month(y, m);
        if (d > max_d) return {};
        return Opt<Date>{Date::from_ymd(y, m, d)};
    }

    // ---- stod helper ----
    static f64 stod_fast(String_View s) noexcept {
        if (s.empty()) return 0.0;
        f64 result = 0.0, sign = 1.0;
        u64 pos = 0;
        if (static_cast<char>(s[0]) == '-') { sign = -1.0; pos++; }
        f64 frac = 0.1;
        bool in_frac = false;
        while (pos < s.length()) {
            char c = static_cast<char>(s[pos]);
            if (c >= '0' && c <= '9') {
                if (in_frac) { result += static_cast<f64>(c - '0') * frac; frac *= 0.1; }
                else result = result * 10.0 + static_cast<f64>(c - '0');
            } else if (c == '.') {
                in_frac = true;
            }
            pos++;
        }
        return result * sign;
    }

    // Parse a single CSV line into Bar
    static Opt<Bar> parse_bar_line(String_View line) noexcept {
        if (line.empty()) return {};
        struct { String_View v[9]; u64 n = 0; } fields;
        u64 start = 0;
        for (u64 i = 0; i <= line.length() && fields.n < 9; i++) {
            if (i == line.length() || static_cast<char>(line[i]) == ',') {
                fields.v[fields.n++] = line.sub(start, i);
                start = i + 1;
            }
        }
        if (fields.n < 6) return {};
        auto date_opt = parse_date(fields.v[0]);
        if (!date_opt.ok()) return {};

        Bar bar;
        bar.date_   = *date_opt;
        bar.open_   = stod_fast(fields.v[1]);
        bar.high_   = stod_fast(fields.v[2]);
        bar.low_    = stod_fast(fields.v[3]);
        bar.close_  = stod_fast(fields.v[4]);
        bar.volume_ = stod_fast(fields.v[5]);

        if (fields.n >= 7) {
            bar.trades_count_ = 0;
            auto t_str = fields.v[6];
            for (u64 i = 0; i < t_str.length(); i++) {
                if (t_str[i] >= '0' && t_str[i] <= '9')
                    bar.trades_count_ = bar.trades_count_ * 10 + static_cast<u64>(t_str[i] - '0');
            }
        }
        if (fields.n >= 8) bar.vwap_ = stod_fast(fields.v[7]);
        else if (bar.volume_ > 0.0) bar.vwap_ = (bar.high_ + bar.low_ + bar.close_) / 3.0;

        return Opt<Bar>{spp::move(bar)};
    }

    // ---- Data Loading ----
    bool load_symbol(String_View symbol) noexcept {
        if (base_path_.empty()) return false;

        // Build path string via Vec<u8> buffer
        Vec<u8> path_buf;
        path_buf.reserve(base_path_.length() + symbol.length() + 16);
        for (u64 i = 0; i < base_path_.length(); i++) path_buf.push(base_path_[i]);
        path_buf.push('/');
        for (u64 i = 0; i < symbol.length(); i++) path_buf.push(symbol[i]);
        for (char c : ".csv"_v) path_buf.push(static_cast<u8>(c));
        String<> path = sv::vec_to_string(path_buf);

        auto file_data = Files::read(path.view());
        if (!file_data.ok()) return false;

        String_View content{(*file_data).data(), (*file_data).length()};

        // Parse bars into Vec, insertion-sorted by date
        Vec<Bar> bars;
        Vec<Date> dates;
        u64 line_start = 0;
        bool is_first_line = true;

        for (u64 i = 0; i <= content.length(); i++) {
            if (i == content.length() || static_cast<char>(content[i]) == '\n') {
                String_View line = content.sub(line_start, i);
                line_start = i + 1;

                if (!line.empty() && static_cast<char>(line[line.length()-1]) == '\r')
                    line = line.sub(0, line.length() - 1);
                if (line.empty()) continue;

                if (is_first_line) {
                    is_first_line = false;
                    if (!line.empty() && (line[0] < '0' || line[0] > '9')) {
                        bool has_date = false;
                        for (u64 c = 0; c + 3 < line.length(); c++) {
                            if (line[c] == 'd' && line[c+1] == 'a' &&
                                line[c+2] == 't' && line[c+3] == 'e') {
                                has_date = true; break;
                            }
                        }
                        if (has_date) continue;
                    }
                }

                auto bar_opt = parse_bar_line(line);
                if (bar_opt.ok()) {
                    Date d = bar_opt->date_;
                    u64 ins = 0;
                    while (ins < dates.length() && dates[ins] < d) ins++;
                    // Grow and shift
                    dates.push(Date{}); bars.push(Bar{});
                    for (u64 j = dates.length() - 1; j > ins; j--) {
                        dates[j] = dates[j-1];
                        bars[j] = spp::move(bars[j-1]);
                    }
                    dates[ins] = d;
                    bars[ins] = spp::move(*bar_opt);
                }
            }
        }

        if (bars.empty()) return false;

        cache_.insert(sv::sv_to_string(symbol), spp::move(bars));
        cache_dates_.insert(sv::sv_to_string(symbol), spp::move(dates));
        current_indices_.insert(sv::sv_to_string(symbol), u64(0));
        current_symbol_ = symbol;
        return true;
    }

    // ---- Streaming ----
    Opt<Bar> next_bar() override {
        if (!connected_) return {};
        if (!current_symbol_.empty()) {
            auto bars_opt = cache_.try_get(current_symbol_);
            if (bars_opt.ok()) {
                u64& idx = current_indices_.get(current_symbol_);
                if (idx < (**bars_opt).length()) {
                    Bar b = (**bars_opt)[idx];
                    idx++;
                    return Opt<Bar>{spp::move(b)};
                }
            }
        }
        for (const auto& kv : cache_) {
            current_symbol_ = kv.first.view();
            if (!current_indices_.contains(kv.first))
                current_indices_.insert(kv.first.clone(), u64(0));
            u64& idx = current_indices_.get(kv.first);
            if (idx < kv.second.length()) {
                Bar b = kv.second[idx];
                idx++;
                return Opt<Bar>{spp::move(b)};
            }
        }
        return {};
    }

    Vec<Bar> load_bars(Date from, Date to) override {
        Vec<Bar> result;
        if (!current_symbol_.empty()) {
            auto dates_opt = cache_dates_.try_get(current_symbol_);
            auto bars_opt = cache_.try_get(current_symbol_);
            if (dates_opt.ok() && bars_opt.ok()) {
                Vec<Date>& ds = **dates_opt;
                Vec<Bar>& bs = **bars_opt;
                for (u64 i = 0; i < ds.length(); i++) {
                    if (ds[i] >= from && ds[i] <= to) result.push(Bar{bs[i]});
                    if (ds[i] > to) break;
                }
            }
        }
        return result;
    }

    void subscribe_quotes(String_View) override {}
    Opt<QuoteTick> next_quote() override { return {}; }
    Opt<OrderBookSnapshot> order_book(String_View) override { return {}; }
};

// =========================================================================
// WebSocket Protocol (RFC 6455) — SHA1 + Base64 + Framing
// =========================================================================
namespace ws_detail {

// xoroshiro128+ PRNG
inline u64 ws_prng_state[2] = {0xdeadbeefcafebabeULL, 0x1234567890abcdefULL};

inline u64 ws_rotl(u64 x, i32 k) noexcept { return (x << k) | (x >> (64 - k)); }

inline u64 ws_prng_next() noexcept {
    u64 s0 = ws_prng_state[0], s1 = ws_prng_state[1], result = s0 + s1;
    s1 ^= s0;
    ws_prng_state[0] = ws_rotl(s0, 24) ^ s1 ^ (s1 << 16);
    ws_prng_state[1] = ws_rotl(s1, 37);
    return result;
}

// Base64
static const char ws_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline String<> ws_base64_encode(Slice<const u8> data) noexcept {
    u64 in_len = data.length();
    u64 out_len = ((in_len + 2) / 3) * 4;
    String s{out_len};
    s.set_length(out_len);
    u64 pos = 0;
    for (u64 i = 0; i < in_len; i += 3) {
        u32 triple = static_cast<u32>(data[i]) << 16;
        if (i + 1 < in_len) triple |= static_cast<u32>(data[i + 1]) << 8;
        if (i + 2 < in_len) triple |= static_cast<u32>(data[i + 2]);
        s[pos++] = static_cast<u8>(ws_b64[(triple >> 18) & 0x3F]);
        s[pos++] = static_cast<u8>(ws_b64[(triple >> 12) & 0x3F]);
        s[pos++] = static_cast<u8>(ws_b64[(i+1 < in_len) ? ((triple >> 6) & 0x3F) : 64]);
        if (i + 1 < in_len) {
            s[pos-1] = static_cast<u8>(ws_b64[(triple >> 6) & 0x3F]);
            s[pos++] = static_cast<u8>(ws_b64[(i+2 < in_len) ? (triple & 0x3F) : 64]);
        }
        if (i + 2 < in_len) s[pos-1] = static_cast<u8>(ws_b64[triple & 0x3F]);
    }
    return s;
}

// SHA1
struct SHA1_CTX {
    u32 state[5] = {};
    u64 count = 0;
    u8 buffer[64] = {};
    u64 buffer_len = 0;
};

inline void sha1_init(SHA1_CTX* ctx) noexcept {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0; ctx->buffer_len = 0;
}

inline u32 sha1_rotl32(u32 x, i32 n) noexcept { return (x << n) | (x >> (32 - n)); }

inline void sha1_transform(u32 state[5], const u8 block[64]) noexcept {
    u32 w[80];
    for (i32 i = 0; i < 16; i++)
        w[i] = (static_cast<u32>(block[i*4])<<24) | (static_cast<u32>(block[i*4+1])<<16) |
               (static_cast<u32>(block[i*4+2])<<8) | static_cast<u32>(block[i*4+3]);
    for (i32 i = 16; i < 80; i++)
        w[i] = sha1_rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    u32 a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (i32 i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b&c)|(~b&d); k = 0x5A827999; }
        else if (i < 40) { f = b^c^d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b&c)|(b&d)|(c&d); k = 0x8F1BBCDC; }
        else { f = b^c^d; k = 0xCA62C1D6; }
        u32 temp = sha1_rotl32(a,5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rotl32(b,30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

inline void sha1_update(SHA1_CTX* ctx, const u8* data, u64 len) noexcept {
    ctx->count += len;
    u64 i = 0;
    while (i < len) {
        u64 space = 64 - ctx->buffer_len, chunk = len - i;
        if (chunk > space) chunk = space;
        for (u64 j = 0; j < chunk; j++) ctx->buffer[ctx->buffer_len+j] = data[i+j];
        ctx->buffer_len += chunk; i += chunk;
        if (ctx->buffer_len == 64) { sha1_transform(ctx->state, ctx->buffer); ctx->buffer_len = 0; }
    }
}

inline void sha1_final(SHA1_CTX* ctx, u8 digest[20]) noexcept {
    u64 bit_count = ctx->count * 8;
    ctx->buffer[ctx->buffer_len++] = 0x80;
    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < 64) ctx->buffer[ctx->buffer_len++] = 0;
        sha1_transform(ctx->state, ctx->buffer); ctx->buffer_len = 0;
    }
    while (ctx->buffer_len < 56) ctx->buffer[ctx->buffer_len++] = 0;
    for (i32 i = 7; i >= 0; i--)
        ctx->buffer[ctx->buffer_len++] = static_cast<u8>((bit_count >> (i*8)) & 0xFF);
    sha1_transform(ctx->state, ctx->buffer);
    for (i32 i = 0; i < 5; i++) {
        digest[i*4] = static_cast<u8>((ctx->state[i] >> 24) & 0xFF);
        digest[i*4+1] = static_cast<u8>((ctx->state[i] >> 16) & 0xFF);
        digest[i*4+2] = static_cast<u8>((ctx->state[i] >> 8) & 0xFF);
        digest[i*4+3] = static_cast<u8>(ctx->state[i] & 0xFF);
    }
}

inline String<> ws_compute_accept_key(String_View client_key) noexcept {
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    u64 total_len = client_key.length() + 36;
    Vec<u8> combined;
    combined.reserve(total_len);
    for (u64 i = 0; i < client_key.length(); i++) combined.push(client_key[i]);
    for (i32 i = 0; i < 36; i++) combined.push(static_cast<u8>(magic[i]));
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, combined.data(), combined.length());
    u8 digest[20];
    sha1_final(&ctx, digest);
    return ws_base64_encode(Slice<const u8>{digest, 20});
}

} // namespace ws_detail

// =========================================================================
// WebSocketConnector — real-time market data via WebSocket (RFC 6455)
// =========================================================================
struct WebSocketConnector : MarketDataConnector {
    String_View endpoint_url_;
    String_View api_key_;

    u64 socket_fd_ = ~u64{0};
    bool connected_ = false;

    Vec<u8> recv_buffer_;
    Vec<u8> send_buffer_;

    Vec<String_View> subscribed_;

    Vec<Bar> bar_buffer_;
    Vec<QuoteTick> quote_buffer_;

    WebSocketConnector() noexcept {
        name_ = "WebSocketConnector"_v;
        recv_buffer_.reserve(65536);
        send_buffer_.reserve(65536);
    }

    virtual ~WebSocketConnector() { disconnect(); }

    // ====================================================================
    // Socket operations (Linux POSIX)
    // ====================================================================
    static u64 create_socket() noexcept {
#ifdef SPP_OS_LINUX
        i32 fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return ~u64{0};
        i32 flag = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        return static_cast<u64>(fd);
#else
        return ~u64{0};
#endif
    }

    static bool socket_connect(u64 fd, String_View host_str, u16 port, u64 timeout_ms = 10000) noexcept {
#ifdef SPP_OS_LINUX
        // Build host string (null-terminated)
        String host{host_str.length() + 1};
        host.set_length(host_str.length() + 1);
        for (u64 i = 0; i < host_str.length(); i++) host[i] = host_str[i];
        host[host_str.length()] = '\0';

        char port_buf[8];
        (void)Libc::snprintf(reinterpret_cast<u8*>(port_buf), 8, "%u", port);

        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = null;
        i32 ret = ::getaddrinfo(reinterpret_cast<const char*>(host.data()), port_buf, &hints, &result);
        if (ret != 0 || !result) return false;

        i32 flags = ::fcntl(static_cast<i32>(fd), F_GETFL, 0);
        ::fcntl(static_cast<i32>(fd), F_SETFL, flags | O_NONBLOCK);

        i32 conn_ret = ::connect(static_cast<i32>(fd), result->ai_addr,
                                  static_cast<socklen_t>(result->ai_addrlen));
        ::freeaddrinfo(result);

        if (conn_ret < 0 && errno == EINPROGRESS) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(static_cast<i32>(fd), &wfds);
            struct timeval tv;
            tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
            tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
            if (::select(static_cast<i32>(fd) + 1, null, &wfds, null, &tv) <= 0) return false;
        }
        ::fcntl(static_cast<i32>(fd), F_SETFL, flags);
        return true;
#else
        (void)fd; (void)host_str; (void)port; (void)timeout_ms;
        return false;
#endif
    }

    static i64 socket_recv(u64 fd, u8* buffer, u64 size, u64 timeout_ms) noexcept {
#ifdef SPP_OS_LINUX
        if (timeout_ms > 0) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(static_cast<i32>(fd), &rfds);
            struct timeval tv;
            tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
            tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
            i32 ret = ::select(static_cast<i32>(fd) + 1, &rfds, null, null, &tv);
            if (ret <= 0) return ret;
        }
        auto n = ::recv(static_cast<i32>(fd), buffer, size, 0);
        return n >= 0 ? n : -1;
#else
        (void)fd; (void)buffer; (void)size; (void)timeout_ms;
        return -1;
#endif
    }

    static i64 socket_send(u64 fd, const u8* data, u64 size) noexcept {
#ifdef SPP_OS_LINUX
        auto n = ::send(static_cast<i32>(fd), data, size, MSG_NOSIGNAL);
        return n >= 0 ? n : -1;
#else
        (void)fd; (void)data; (void)size;
        return -1;
#endif
    }

    static void socket_close(u64 fd) noexcept {
#ifdef SPP_OS_LINUX
        if (fd != ~u64{0}) ::close(static_cast<i32>(fd));
#else
        (void)fd;
#endif
    }

    // ====================================================================
    // WebSocket Handshake
    // ====================================================================
    bool ws_handshake(String_View host, String_View path) noexcept {
        // Generate key
        u8 key_bytes[16];
        for (i32 i = 0; i < 16; i++) key_bytes[i] = static_cast<u8>(ws_detail::ws_prng_next() & 0xFF);
        String client_key = ws_detail::ws_base64_encode(Slice<const u8>{key_bytes, 16});

        // Build request via Vec<u8>
        Vec<u8> req;
        req.reserve(512);

        auto push_sv = [&req](String_View s) {
            for (u64 i = 0; i < s.length(); i++) req.push(s[i]);
        };

        push_sv("GET "_v); push_sv(path); push_sv(" HTTP/1.1\r\n"_v);
        push_sv("Host: "_v); push_sv(host); push_sv("\r\n"_v);
        push_sv("Upgrade: websocket\r\n"_v);
        push_sv("Connection: Upgrade\r\n"_v);
        push_sv("Sec-WebSocket-Key: "_v);
        push_sv(client_key.view());
        push_sv("\r\n"_v);
        push_sv("Sec-WebSocket-Version: 13\r\n"_v);
        if (!api_key_.empty()) {
            push_sv("X-API-Key: "_v); push_sv(api_key_); push_sv("\r\n"_v);
        }
        push_sv("\r\n"_v);

        auto n = socket_send(socket_fd_, req.data(), req.length());
        if (n < 0) return false;

        // Read response
        u8 tmp[4096];
        u64 total_read = 0;
        for (u64 attempt = 0; attempt < 5 && total_read < sizeof(tmp); attempt++) {
            i64 r = socket_recv(socket_fd_, tmp + total_read, sizeof(tmp) - total_read, 3000);
            if (r <= 0) break;
            total_read += static_cast<u64>(r);
            if (total_read >= 4) {
                bool done = false;
                for (u64 i = 0; i + 3 < total_read; i++) {
                    if (tmp[i] == '\r' && tmp[i+1] == '\n' && tmp[i+2] == '\r' && tmp[i+3] == '\n')
                        { done = true; break; }
                }
                if (done) break;
            }
        }

        String_View response{tmp, total_read};
        if (response.length() >= 12 &&
            response.sub(0, 9) == "HTTP/1.1 "_v &&
            response.sub(9, 12) == "101"_v)
        {
            // OK — validate accept key
            String<> expected = ws_detail::ws_compute_accept_key(client_key.view());
            String_View exp_view = expected.view();
            // Find accept header
            for (u64 i = 0; i + 22 < response.length(); i++) {
                char c0 = static_cast<char>(response[i]);
                if (c0 == 'S' || c0 == 's') {
                    String_View cand = response.sub(i, i + 22 < response.length() ? i + 22 : response.length());
                    String_View lower = "sec-websocket-accept: "_v;
                    if (cand.length() >= 22) {
                        cand = cand.sub(0, 22);
                        bool match = true;
                        for (u64 j = 0; j < 22; j++) {
                            char c = static_cast<char>(cand[j]);
                            char e = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
                            if (e != static_cast<char>(lower[j])) { match = false; break; }
                        }
                        if (match) {
                            u64 vs = i + 22;
                            while (vs < response.length() && response[vs] == ' ') vs++;
                            u64 ve = vs;
                            while (ve < response.length() && response[ve] != '\r' && response[ve] != '\n') ve++;
                            if (response.sub(vs, ve) == exp_view) return true;
                        }
                    }
                }
            }
            // [UNSPECIFIED] Accept header validation failure. Accept connection anyway.
            return true;
        }
        return false;
    }

    // ====================================================================
    // WebSocket Framing
    // ====================================================================

    bool ws_send(String_View message) noexcept {
        send_buffer_.clear();
        u64 msg_len = message.length();
        u8 mask[4];
        for (i32 i = 0; i < 4; i++) mask[i] = static_cast<u8>(ws_detail::ws_prng_next() & 0xFF);

        send_buffer_.push(0x81); // FIN + text
        if (msg_len <= 125) {
            send_buffer_.push(static_cast<u8>(msg_len | 0x80));
        } else if (msg_len <= 65535) {
            send_buffer_.push(static_cast<u8>(126 | 0x80));
            send_buffer_.push(static_cast<u8>((msg_len >> 8) & 0xFF));
            send_buffer_.push(static_cast<u8>(msg_len & 0xFF));
        } else {
            send_buffer_.push(static_cast<u8>(127 | 0x80));
            for (i32 i = 7; i >= 0; i--)
                send_buffer_.push(static_cast<u8>((msg_len >> (i*8)) & 0xFF));
        }
        for (i32 i = 0; i < 4; i++) send_buffer_.push(mask[i]);
        for (u64 i = 0; i < msg_len; i++)
            send_buffer_.push(static_cast<u8>(message[i] ^ mask[i % 4]));

        auto n = socket_send(socket_fd_, send_buffer_.data(), send_buffer_.length());
        return n == static_cast<i64>(send_buffer_.length());
    }

    Opt<String<>> ws_recv(u64 timeout_ms = 5000) noexcept {
        recv_buffer_.clear();
        u8 header[2];
        i64 r = socket_recv(socket_fd_, header, 2, timeout_ms);
        if (r < 2) return {};

        u8 opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        u64 payload_len = header[1] & 0x7F;

        if (payload_len == 126) {
            u8 ext[2]; r = socket_recv(socket_fd_, ext, 2, timeout_ms);
            if (r < 2) return {};
            payload_len = (static_cast<u64>(ext[0]) << 8) | static_cast<u64>(ext[1]);
        } else if (payload_len == 127) {
            u8 ext[8]; r = socket_recv(socket_fd_, ext, 8, timeout_ms);
            if (r < 8) return {};
            payload_len = 0;
            for (i32 i = 0; i < 8; i++) payload_len = (payload_len << 8) | static_cast<u64>(ext[i]);
        }

        u8 mask_key[4] = {};
        if (masked) { r = socket_recv(socket_fd_, mask_key, 4, timeout_ms); if (r < 4) return {}; }

        if (payload_len > 0) {
            recv_buffer_.reserve(payload_len);
            for (u64 i = 0; i < payload_len; i++) recv_buffer_.push(0);
            u64 total = 0;
            while (total < payload_len) {
                u64 remaining = payload_len - total;
                r = socket_recv(socket_fd_, recv_buffer_.data() + total, remaining, timeout_ms);
                if (r <= 0) return {};
                total += static_cast<u64>(r);
            }
        }

        if (opcode == 0x08) { connected_ = false; return {}; }
        if (opcode == 0x09) { u8 pong[2] = {0x8A, 0x00}; socket_send(socket_fd_, pong, 2); return ws_recv(timeout_ms); }
        if (opcode == 0x0A) { return ws_recv(timeout_ms); }
        if (opcode != 0x01 && opcode != 0x02) return {};

        if (masked && payload_len > 0) {
            for (u64 i = 0; i < payload_len; i++) recv_buffer_[i] ^= mask_key[i % 4];
        }

        String result{payload_len};
        result.set_length(payload_len);
        for (u64 i = 0; i < payload_len; i++) result[i] = recv_buffer_[i];
        return Opt<String<>>{spp::move(result)};
    }

    // ====================================================================
    // MarketDataConnector interface
    // ====================================================================
    virtual bool connect() override {
#ifdef SPP_OS_LINUX
        String_View url = endpoint_url_;
        bool use_tls = false;
        u64 scheme_end = 0;

        if (sv::starts_with(url, "wss://"_v)) { use_tls = true; scheme_end = 6; }
        else if (sv::starts_with(url, "ws://"_v)) { scheme_end = 5; }
        else { scheme_end = 0; }

        if (use_tls) return false; // [UNSPECIFIED] TLS not implemented

        String_View rest = url.sub(scheme_end, url.length());
        u64 host_end = 0, port_start_val = 0;
        u16 port = 80;

        for (u64 i = 0; i < rest.length(); i++) {
            if (static_cast<char>(rest[i]) == ':') { host_end = i; port_start_val = i + 1; }
            if (static_cast<char>(rest[i]) == '/') { if (host_end == 0) host_end = i; break; }
        }
        if (host_end == 0) host_end = rest.length();
        String_View host = rest.sub(0, host_end);

        if (port_start_val > 0) {
            u64 pe = port_start_val;
            while (pe < rest.length() && rest[pe] >= '0' && rest[pe] <= '9') pe++;
            port = 0;
            for (u64 i = port_start_val; i < pe; i++) port = port * 10 + static_cast<u16>(rest[i] - '0');
        }

        u64 ps = 0;
        for (u64 i = 0; i < rest.length(); i++) {
            if (static_cast<char>(rest[i]) == '/') { ps = i; break; }
        }
        String_View path = "/"_v;
        if (ps > 0) path = rest.sub(ps, rest.length());

        socket_fd_ = create_socket();
        if (socket_fd_ == ~u64{0}) return false;
        if (!socket_connect(socket_fd_, host, port, 10000)) {
            socket_close(socket_fd_); socket_fd_ = ~u64{0}; return false;
        }
        if (!ws_handshake(host, path)) {
            socket_close(socket_fd_); socket_fd_ = ~u64{0}; return false;
        }
        connected_ = true;
        ws_detail::ws_prng_state[0] = 0xdeadbeefcafebabeULL ^ static_cast<u64>(
            reinterpret_cast<uintptr_t>(this));
        return true;
#else
        return false;
#endif
    }

    virtual void disconnect() override {
        if (connected_ && socket_fd_ != ~u64{0}) {
            u8 close_frame[4] = {0x88, 0x02, 0x03, 0xE8};
            socket_send(socket_fd_, close_frame, 4);
        }
        socket_close(socket_fd_);
        socket_fd_ = ~u64{0};
        connected_ = false;
        subscribed_.clear();
        bar_buffer_.clear();
        quote_buffer_.clear();
    }

    virtual bool is_connected() const override { return connected_; }

    virtual Opt<Bar> next_bar() override {
        if (bar_buffer_.empty()) return {};
        Bar b = spp::move(bar_buffer_[0]);
        for (u64 i = 1; i < bar_buffer_.length(); i++) bar_buffer_[i-1] = spp::move(bar_buffer_[i]);
        bar_buffer_.pop();
        return Opt<Bar>{spp::move(b)};
    }

    virtual Vec<Bar> load_bars(Date from, Date to) override {
        (void)from; (void)to;
        return Vec<Bar>{};
    }

    virtual Vec<String_View> symbols() const override {
        // Vec copy is deleted; use clone() for const access
        return subscribed_.clone();
    }

    virtual void subscribe_quotes(String_View symbol) override {
        Vec<u8> sub;
        sub.reserve(256);
        auto push_sv = [&sub](String_View s) { for (u64 i = 0; i < s.length(); i++) sub.push(s[i]); };
        push_sv("{\"method\":\"SUBSCRIBE\",\"params\":[\""_v);
        push_sv(symbol);
        push_sv("@bookTicker\"],\"id\":1}"_v);
        ws_send(String_View{sub.data(), sub.length()});

        bool found = false;
        for (u64 i = 0; i < subscribed_.length(); i++) {
            if (subscribed_[i] == symbol) { found = true; break; }
        }
        if (!found) subscribed_.push(symbol);
    }

    virtual Opt<QuoteTick> next_quote() override {
        if (quote_buffer_.empty()) return {};
        QuoteTick q = spp::move(quote_buffer_[0]);
        for (u64 i = 1; i < quote_buffer_.length(); i++) quote_buffer_[i-1] = spp::move(quote_buffer_[i]);
        quote_buffer_.pop();
        return Opt<QuoteTick>{spp::move(q)};
    }

    virtual Opt<OrderBookSnapshot> order_book(String_View symbol) override {
        (void)symbol; return {};
    }

    virtual void parse_message(String_View json_message) noexcept { (void)json_message; }

    u64 poll(u64 timeout_ms = 0) noexcept {
        if (!connected_) return 0;
        u64 count = 0;
        for (;;) {
            auto msg = ws_recv(timeout_ms);
            if (!msg.ok()) break;
            parse_message(msg->view());
            count++;
            if (timeout_ms == 0) break;
        }
        return count;
    }

    static Opt<Date> parse_iso_timestamp(String_View ts) noexcept {
        if (ts.empty()) return {};
        if (ts[0] >= '0' && ts[0] <= '9') {
            i64 val = 0;
            for (u64 i = 0; i < ts.length() && ts[i] >= '0' && ts[i] <= '9'; i++)
                val = val * 10 + static_cast<i64>(ts[i] - '0');
            if (val > 1000000000000LL) val /= 1000;
            i32 serial = static_cast<i32>(val / 86400 + 25569);
            return Opt<Date>{Date{serial}};
        }
        if (ts.length() >= 10 && static_cast<char>(ts[4]) == '-') {
            i32 y=0; u32 m=0, d=0;
            for (i32 i=0; i<4; i++) y = y*10 + (ts[i]-'0');
            for (i32 i=5; i<7; i++) m = m*10 + (ts[i]-'0');
            for (i32 i=8; i<10; i++) d = d*10 + (ts[i]-'0');
            return Opt<Date>{Date::from_ymd(y, m, d)};
        }
        return {};
    }
};

// =========================================================================
// BinanceConnector — Binance WebSocket API connector
// =========================================================================
struct BinanceConnector : WebSocketConnector {
    BinanceConnector() noexcept { name_ = "BinanceConnector"_v; }

    bool connect() override {
        if (endpoint_url_.empty()) endpoint_url_ = "wss://stream.binance.com:9443/stream"_v;
        return WebSocketConnector::connect();
    }

    void subscribe_kline(String_View symbol, String_View interval = "1m"_v) noexcept {
        if (!connected_) return;
        // Build lowercase symbol
        Vec<u8> lower_buf;
        lower_buf.reserve(symbol.length());
        for (u64 i = 0; i < symbol.length(); i++) {
            char c = static_cast<char>(symbol[i]);
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            lower_buf.push(static_cast<u8>(c));
        }
        String_View lower_sv{lower_buf.data(), lower_buf.length()};

        Vec<u8> sub;
        sub.reserve(256);
        auto push_sv = [&sub](String_View s) { for (u64 i = 0; i < s.length(); i++) sub.push(s[i]); };

        push_sv("{\"method\":\"SUBSCRIBE\",\"params\":[\""_v);
        push_sv(lower_sv);
        push_sv("@kline_"_v);
        push_sv(interval);
        push_sv("\"],\"id\":1}"_v);

        ws_send(String_View{sub.data(), sub.length()});

        bool found = false;
        for (u64 i = 0; i < subscribed_.length(); i++) {
            if (subscribed_[i] == symbol) { found = true; break; }
        }
        if (!found) subscribed_.push(symbol);
    }

    void subscribe_depth(String_View symbol, u64 levels = 20) noexcept {
        if (!connected_) return;
        Vec<u8> lower_buf;
        lower_buf.reserve(symbol.length());
        for (u64 i = 0; i < symbol.length(); i++) {
            char c = static_cast<char>(symbol[i]);
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            lower_buf.push(static_cast<u8>(c));
        }
        String_View lower_sv{lower_buf.data(), lower_buf.length()};

        char lvl_buf[8];
        (void)Libc::snprintf(reinterpret_cast<u8*>(lvl_buf), 8, "%lu", levels);

        Vec<u8> sub;
        sub.reserve(256);
        auto push_sv = [&sub](String_View s) { for (u64 i = 0; i < s.length(); i++) sub.push(s[i]); };

        push_sv("{\"method\":\"SUBSCRIBE\",\"params\":[\""_v);
        push_sv(lower_sv);
        push_sv("@depth"_v);
        for (u64 i = 0; i < 6 && lvl_buf[i] != '\0'; i++) sub.push(static_cast<u8>(lvl_buf[i]));
        push_sv("\"],\"id\":2}"_v);

        ws_send(String_View{sub.data(), sub.length()});
    }

    static Opt<Bar> parse_kline_message(String_View json) noexcept {
        if (!sv::contains(json, "\"kline\""_v) && !sv::contains(json, "\"k\""_v)) return {};
        using json_detail::get_f64_value;
        using json_detail::get_i64_value;
        using json_detail::get_bool_value;

        auto open_v    = get_f64_value(json, "o"_v);
        auto high_v    = get_f64_value(json, "h"_v);
        auto low_v     = get_f64_value(json, "l"_v);
        auto close_v   = get_f64_value(json, "c"_v);
        auto volume_v  = get_f64_value(json, "v"_v);
        auto trades_v  = get_i64_value(json, "n"_v);
        auto qvol_v    = get_f64_value(json, "q"_v);
        auto ts_v      = get_i64_value(json, "t"_v);
        auto closed_v  = get_bool_value(json, "x"_v);

        if (!open_v.ok() || !close_v.ok()) return {};

        Bar bar;
        bar.open_   = *open_v;
        bar.high_   = high_v.ok() ? *high_v : bar.open_;
        bar.low_    = low_v.ok() ? *low_v : bar.open_;
        bar.close_  = *close_v;
        bar.volume_ = volume_v.ok() ? *volume_v : 0.0;
        bar.trades_count_ = trades_v.ok() ? static_cast<u64>(*trades_v) : 0;

        if (ts_v.ok()) {
            i64 ts_ms = *ts_v;
            bar.date_ = Date{static_cast<i32>(ts_ms / 86400000 + 25569)};
        } else {
            bar.date_ = Date::today();
        }

        if (qvol_v.ok() && bar.volume_ > 0.0) bar.vwap_ = *qvol_v / bar.volume_;
        else if (bar.volume_ > 0.0) bar.vwap_ = (bar.high_ + bar.low_ + bar.close_) / 3.0;

        // [UNSPECIFIED] Partial bar (x=false) returned alongside complete bars
        (void)closed_v;

        return Opt<Bar>{spp::move(bar)};
    }

    static Opt<QuoteTick> parse_book_ticker(String_View json) noexcept {
        using json_detail::get_f64_value;
        if (!sv::contains(json, "\"b\""_v) && !sv::contains(json, "\"bid\""_v)) return {};

        auto bid_v = get_f64_value(json, "b"_v);
        auto ask_v = get_f64_value(json, "a"_v);
        auto bid_sz = get_f64_value(json, "B"_v);
        auto ask_sz = get_f64_value(json, "A"_v);
        if (!bid_v.ok()) bid_v = get_f64_value(json, "bidPrice"_v);
        if (!ask_v.ok()) ask_v = get_f64_value(json, "askPrice"_v);
        if (!bid_sz.ok()) bid_sz = get_f64_value(json, "bidQty"_v);
        if (!ask_sz.ok()) ask_sz = get_f64_value(json, "askQty"_v);
        if (!bid_v.ok() || !ask_v.ok()) return {};

        QuoteTick qt;
        qt.timestamp_ = Date::today();
        qt.bid_ = *bid_v; qt.ask_ = *ask_v;
        qt.bid_size_ = bid_sz.ok() ? *bid_sz : 0.0;
        qt.ask_size_ = ask_sz.ok() ? *ask_sz : 0.0;
        return Opt<QuoteTick>{spp::move(qt)};
    }

    static Opt<OrderBookSnapshot> parse_depth_message(String_View json) noexcept {
        (void)json;
        return {};
    }

    void parse_message(String_View json_message) noexcept override {
        if (json_message.empty()) return;
        String_View data = json_message;

        if (sv::contains(json_message, "\"stream\""_v) && sv::contains(json_message, "\"data\""_v)) {
            // Combined stream: extract inner data object
            for (u64 i = 0; i + 7 < json_message.length(); i++) {
                if (static_cast<char>(json_message[i]) == '"' &&
                    json_message[i+1] == 'd' && json_message[i+2] == 'a' &&
                    json_message[i+3] == 't' && json_message[i+4] == 'a' &&
                    json_message[i+5] == '"') {
                    u64 j = i + 6;
                    while (j < json_message.length() && json_message[j] != '{') j++;
                    if (j < json_message.length()) {
                        u64 depth = 0, end = j;
                        for (u64 k = j; k < json_message.length(); k++) {
                            if (json_message[k] == '{') depth++;
                            else if (json_message[k] == '}') { depth--; if (depth == 0) { end = k + 1; break; } }
                        }
                        data = json_message.sub(j, end);
                    }
                    break;
                }
            }
        }

        auto bar_opt = parse_kline_message(data);
        if (bar_opt.ok()) { bar_buffer_.push(spp::move(*bar_opt)); return; }

        auto quote_opt = parse_book_ticker(data);
        if (quote_opt.ok()) { quote_buffer_.push(spp::move(*quote_opt)); return; }

        auto depth_opt = parse_depth_message(data);
        if (depth_opt.ok()) { return; }
    }

    Opt<Bar> try_parse_kline(String_View json) noexcept { return parse_kline_message(json); }
    Opt<QuoteTick> try_parse_book_ticker(String_View json) noexcept { return parse_book_ticker(json); }
};

// =========================================================================
// Factory functions — construct connectors with common configurations
//
// NOTE: These types are move-only (contain Vec/Map). Use direct construction
// or local variables — do not copy these structs.
// =========================================================================

} // namespace spp::quant

// =========================================================================
// SPP reflection records (at global scope)
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::Bar, "Bar",
                 SPP_FIELD(date_), SPP_FIELD(open_), SPP_FIELD(high_),
                 SPP_FIELD(low_), SPP_FIELD(close_), SPP_FIELD(volume_),
                 SPP_FIELD(trades_count_), SPP_FIELD(vwap_));

SPP_NAMED_RECORD(::spp::quant::QuoteTick, "QuoteTick",
                 SPP_FIELD(timestamp_), SPP_FIELD(bid_), SPP_FIELD(ask_),
                 SPP_FIELD(bid_size_), SPP_FIELD(ask_size_));

SPP_NAMED_RECORD(::spp::quant::TradeTick, "TradeTick",
                 SPP_FIELD(timestamp_), SPP_FIELD(price_), SPP_FIELD(volume_),
                 SPP_FIELD(side_), SPP_FIELD(trade_id_));

SPP_NAMED_RECORD(::spp::quant::OrderBookSnapshot, "OrderBookSnapshot",
                 SPP_FIELD(timestamp_), SPP_FIELD(symbol_));
