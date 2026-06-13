#pragma once

#include <spp/concurrency/thread.h>
#include <spp/exchange/binance.h>
#include <spp/io/stream.h>
#include <spp/protocol/http.h>
#include <spp/reflection/json.h>

#include <binance/clock.h>
#include <binance/models.h>
#include <binance/rate_limiter.h>
#include <binance/time_sync.h>

namespace spp::App::Binance {

// Free functions templated on Net::Byte_Stream. They encapsulate the
// limiter-check + request-build + fetch + limiter-update + JSON-parse cycle
// so the Client facade and the Memory_Stream-driven unit tests share the
// same code path.
//
// Endpoint weights (per Binance docs as of 2026): /api/v3/time = 1,
// /api/v3/ticker/price (single symbol) = 2, GET /api/v3/account = 20,
// POST /api/v3/order = 1.

namespace detail {

[[nodiscard]] inline String_View i64_to_view(i64 v, char (&buf)[32]) noexcept {
    i32 n = Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf), "%lld",
                           static_cast<long long>(v));
    if(n <= 0) return ""_v;
    return String_View{reinterpret_cast<const u8*>(buf), static_cast<u64>(n)};
}

template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Protocol::Http::Fetched_Response<Mdefault>, String_View>
send_throttled(S& stream, const Protocol::Http::Request<Mdefault>& req,
               Rate_Limiter& limiter, i64 weight, i64 now_ms) noexcept {
    i64 wait = limiter.wait_required_ms(weight, now_ms);
    if(wait > 0) {
        // Sleep with ms granularity. Thread::sleep takes ms.
        Thread::sleep(static_cast<u64>(wait));
        now_ms += wait;
    }
    auto fetched = Protocol::Http::fetch(stream, req);
    if(!fetched.ok()) {
        return Result<Protocol::Http::Fetched_Response<Mdefault>, String_View>::err(
            spp::move(fetched.unwrap_err()));
    }
    limiter.update_from(fetched.unwrap().response, weight, now_ms);
    if(fetched.unwrap().response.status_code >= 400) {
        // Return the response anyway — callers may want to inspect the body
        // for Binance error codes, but we surface a typed error first.
        return Result<Protocol::Http::Fetched_Response<Mdefault>, String_View>::err(
            "http_error_status"_v);
    }
    limiter.clear_cooldown_if_passed(now_ms);
    return Result<Protocol::Http::Fetched_Response<Mdefault>, String_View>::ok(
        spp::move(fetched.unwrap()));
}

template<typename T>
[[nodiscard]] Result<T, String_View>
parse_body(const Protocol::Http::Response<Mdefault>& resp) noexcept {
    String_View body{resp.body.data(), resp.body.length()};
    return Json::parse_result<T>(body);
}

} // namespace detail

// GET /api/v3/time — also a clean RTT probe for Time_Sync::refresh.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Server_Time, String_View>
fetch_server_time(S& stream, String_View host, Rate_Limiter& limiter,
                  i64 now_ms_value) noexcept {
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = "/api/v3/time"_v;
    req.host = host;
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Server_Time, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Server_Time>(fetched.unwrap().response);
}

// GET /api/v3/ticker/price?symbol=<symbol>.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Ticker_Price, String_View>
fetch_ticker_price(S& stream, String_View host, Rate_Limiter& limiter,
                   i64 now_ms_value, String_View symbol) noexcept {
    auto path_root = "/api/v3/ticker/price?symbol="_v.append<Mdefault>(symbol);
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = path_root.view();
    req.host = host;
    constexpr i64 weight = 2;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Ticker_Price, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Ticker_Price>(fetched.unwrap().response);
}

// GET /api/v3/account — SIGNED.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Account_Info, String_View>
fetch_account(S& stream, String_View host, String_View api_key, String_View api_secret,
              Rate_Limiter& limiter, Time_Sync& time, i64 now_ms_value,
              i64 recv_window_ms = 5000) noexcept {
    char ts_buf[32], rw_buf[32];
    auto ts = detail::i64_to_view(time.timestamp_for(now_ms_value), ts_buf);
    auto rw = detail::i64_to_view(recv_window_ms, rw_buf);

    Exchange::Binance::Query_Builder<Mdefault> q;
    q.add("recvWindow"_v, rw);
    q.add("timestamp"_v, ts);

    auto signed_req = Exchange::Binance::signed_request_full<Mdefault>(
        "GET"_v, "/api/v3/account"_v, q.view(), host, api_key, api_secret);

    constexpr i64 weight = 20;
    auto fetched = detail::send_throttled(stream, signed_req.request, limiter, weight,
                                          now_ms_value);
    if(!fetched.ok()) {
        return Result<Account_Info, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Account_Info>(fetched.unwrap().response);
}

// POST /api/v3/order — SIGNED. The body lives in the query string per Binance
// conventions; the signer hashes the whole query and `Order_Request`
// translates to canonical &-joined parameters.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Order_Response, String_View>
place_order(S& stream, String_View host, String_View api_key, String_View api_secret,
            Rate_Limiter& limiter, Time_Sync& time, i64 now_ms_value,
            const Order_Request& order, i64 recv_window_ms = 5000) noexcept {
    Exchange::Binance::Query_Builder<Mdefault> q;
    q.add("symbol"_v, order.symbol);
    q.add("side"_v, wire_name(order.side));
    q.add("type"_v, wire_name(order.type));
    if(order.time_in_force.ok()) {
        q.add("timeInForce"_v, wire_name(*order.time_in_force));
    }
    if(order.quantity.length() > 0) q.add("quantity"_v, order.quantity);
    if(order.price.length() > 0) q.add("price"_v, order.price);
    if(order.stop_price.length() > 0) q.add("stopPrice"_v, order.stop_price);
    if(order.client_order_id.length() > 0) {
        q.add("newClientOrderId"_v, order.client_order_id);
    }

    char ts_buf[32], rw_buf[32];
    q.add("recvWindow"_v, detail::i64_to_view(recv_window_ms, rw_buf));
    q.add("timestamp"_v, detail::i64_to_view(time.timestamp_for(now_ms_value), ts_buf));

    auto signed_req = Exchange::Binance::signed_request_full<Mdefault>(
        "POST"_v, "/api/v3/order"_v, q.view(), host, api_key, api_secret);

    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, signed_req.request, limiter, weight,
                                          now_ms_value);
    if(!fetched.ok()) {
        return Result<Order_Response, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Order_Response>(fetched.unwrap().response);
}

// Parse a JSON number (i64) from the cursor. Consumes digits and optional
// leading '-' sign. Returns the parsed value.
[[nodiscard]] inline Result<i64, String_View> _parse_kline_int(
    Json::detail::Cursor& c) noexcept {
    Json::detail::skip_ws(c);
    if (c.i >= c.s.length()) return Result<i64, String_View>::err("json_eof_in_number"_v);
    bool neg = false;
    if (c.s[c.i] == '-') { neg = true; c.i++; }
    i64 val = 0;
    u64 start = c.i;
    while (c.i < c.s.length()) {
        u8 ch = c.s[c.i];
        if (ch >= '0' && ch <= '9') { val = val * 10 + (ch - '0'); c.i++; }
        else break;
    }
    if (c.i == start) return Result<i64, String_View>::err("json_expected_number"_v);
    return Result<i64, String_View>::ok(neg ? -val : val);
}

// Manually parse Binance's kline array-of-arrays JSON into typed Kline structs.
// Binance format: [[openTime,"open","high","low","close","volume",closeTime,
//   "quoteVol",trades,"takerBuyBase","takerBuyQuote","unused"], ...]
[[nodiscard]] inline Result<Vec<Kline, Mdefault>, String_View> parse_klines(
    String_View body) noexcept {
    Json::detail::Cursor c{body};
    Json::detail::skip_ws(c);
    if (c.i >= c.s.length() || c.s[c.i] != '[')
        return Result<Vec<Kline, Mdefault>, String_View>::err("json_expected_array"_v);
    c.i++; // skip outer '['
    Vec<Kline, Mdefault> out;
    Json::detail::skip_ws(c);
    if (c.i < c.s.length() && c.s[c.i] == ']') { c.i++; return Result<Vec<Kline, Mdefault>, String_View>::ok(spp::move(out)); }
    for (;;) {
        Json::detail::skip_ws(c);
        if (c.i >= c.s.length() || c.s[c.i] != '[')
            return Result<Vec<Kline, Mdefault>, String_View>::err("json_expected_kline_array"_v);
        c.i++; // skip inner '['
        Kline k;
        // Position 0: open_time (i64)
        auto ot = _parse_kline_int(c); if (!ot.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(ot.unwrap_err()));
        k.open_time = ot.unwrap(); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        // Position 1-5: strings (open, high, low, close, volume)
        auto s1 = Json::detail::parse_string(c); if (!s1.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s1.unwrap_err()));
        k.open = spp::move(s1.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        auto s2 = Json::detail::parse_string(c); if (!s2.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s2.unwrap_err()));
        k.high = spp::move(s2.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        auto s3 = Json::detail::parse_string(c); if (!s3.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s3.unwrap_err()));
        k.low = spp::move(s3.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        auto s4 = Json::detail::parse_string(c); if (!s4.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s4.unwrap_err()));
        k.close = spp::move(s4.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        auto s5 = Json::detail::parse_string(c); if (!s5.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s5.unwrap_err()));
        k.volume = spp::move(s5.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        // Position 6: close_time (i64)
        auto ct = _parse_kline_int(c); if (!ct.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(ct.unwrap_err()));
        k.close_time = ct.unwrap(); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        // Position 7: quote_volume (string)
        auto s7 = Json::detail::parse_string(c); if (!s7.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s7.unwrap_err()));
        k.quote_volume = spp::move(s7.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        // Position 8: trades (i32)
        auto tr = _parse_kline_int(c); if (!tr.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(tr.unwrap_err()));
        k.trades = static_cast<i32>(tr.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        // Position 9-11: strings
        auto s9 = Json::detail::parse_string(c); if (!s9.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s9.unwrap_err()));
        k.taker_buy_base = spp::move(s9.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        auto s10 = Json::detail::parse_string(c); if (!s10.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s10.unwrap_err()));
        k.taker_buy_quote = spp::move(s10.unwrap()); Json::detail::skip_ws(c); if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        auto s11 = Json::detail::parse_string(c); if (!s11.ok()) return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(s11.unwrap_err()));
        k.unused = spp::move(s11.unwrap());
        // Consume ']'
        Json::detail::skip_ws(c);
        if (c.i >= c.s.length() || c.s[c.i] != ']')
            return Result<Vec<Kline, Mdefault>, String_View>::err("json_expected_kline_close"_v);
        c.i++;
        out.push(spp::move(k));
        Json::detail::skip_ws(c);
        if (c.i < c.s.length() && c.s[c.i] == ']') { c.i++; return Result<Vec<Kline, Mdefault>, String_View>::ok(spp::move(out)); }
        if (c.i < c.s.length() && c.s[c.i] == ',') c.i++;
        else return Result<Vec<Kline, Mdefault>, String_View>::err("json_expected_comma_or_close"_v);
    }
}

// GET /api/v3/klines — historical candlestick data.
// Binance returns this as a JSON array-of-arrays (not objects), so we parse
// each inner array element-by-element into a typed Kline struct.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Vec<Kline, Mdefault>, String_View>
fetch_klines(S& stream, String_View host, Rate_Limiter& limiter, i64 now_ms_value,
             String_View symbol, String_View interval, u64 limit = 500,
             Opt<i64> start_time = {}, Opt<i64> end_time = {}) noexcept {
    // Build path using Query_Builder for consistency with signed endpoints.
    Exchange::Binance::Query_Builder<Mdefault> q;
    q.add("symbol"_v, symbol);
    q.add("interval"_v, interval);
    char buf[32];
    q.add("limit"_v, detail::i64_to_view(static_cast<i64>(limit), buf));
    if (start_time.ok()) {
        q.add("startTime"_v, detail::i64_to_view(*start_time, buf));
    }
    if (end_time.ok()) {
        q.add("endTime"_v, detail::i64_to_view(*end_time, buf));
    }

    auto path_root = "/api/v3/klines?"_v.append<Mdefault>(q.view());
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = path_root.view();
    req.host = host;

    constexpr i64 weight = 2; // per 500 bars
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if (!fetched.ok()) {
        return Result<Vec<Kline, Mdefault>, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    String_View body{fetched.unwrap().response.body.data(), fetched.unwrap().response.body.length()};
    return parse_klines(body);
}

// GET /api/v3/order — query order status (SIGNED).
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Order_State, String_View>
query_order(S& stream, String_View host, String_View api_key, String_View api_secret,
            Rate_Limiter& limiter, Time_Sync& time, i64 now_ms_value,
            String_View symbol, i64 order_id, i64 recv_window_ms = 5000) noexcept {
    char id_buf[32], ts_buf[32], rw_buf[32];
    auto oid = detail::i64_to_view(order_id, id_buf);

    Exchange::Binance::Query_Builder<Mdefault> q;
    q.add("symbol"_v, symbol);
    q.add("orderId"_v, oid);
    q.add("recvWindow"_v, detail::i64_to_view(recv_window_ms, rw_buf));
    q.add("timestamp"_v, detail::i64_to_view(time.timestamp_for(now_ms_value), ts_buf));

    auto signed_req = Exchange::Binance::signed_request_full<Mdefault>(
        "GET"_v, "/api/v3/order"_v, q.view(), host, api_key, api_secret);

    constexpr i64 weight = 2;
    auto fetched = detail::send_throttled(stream, signed_req.request, limiter, weight, now_ms_value);
    if (!fetched.ok()) {
        return Result<Order_State, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Order_State>(fetched.unwrap().response);
}

// DELETE /api/v3/order — cancel an order (SIGNED).
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Order_State, String_View>
cancel_order(S& stream, String_View host, String_View api_key, String_View api_secret,
             Rate_Limiter& limiter, Time_Sync& time, i64 now_ms_value,
             String_View symbol, i64 order_id, i64 recv_window_ms = 5000) noexcept {
    char id_buf[32], ts_buf[32], rw_buf[32];
    auto oid = detail::i64_to_view(order_id, id_buf);

    Exchange::Binance::Query_Builder<Mdefault> q;
    q.add("symbol"_v, symbol);
    q.add("orderId"_v, oid);
    q.add("recvWindow"_v, detail::i64_to_view(recv_window_ms, rw_buf));
    q.add("timestamp"_v, detail::i64_to_view(time.timestamp_for(now_ms_value), ts_buf));

    auto signed_req = Exchange::Binance::signed_request_full<Mdefault>(
        "DELETE"_v, "/api/v3/order"_v, q.view(), host, api_key, api_secret);

    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, signed_req.request, limiter, weight, now_ms_value);
    if (!fetched.ok()) {
        return Result<Order_State, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Order_State>(fetched.unwrap().response);
}

// GET /api/v3/exchangeInfo — symbol filters, intervals, and metadata.
// Use this to populate Filter_Cache so PRICE_FILTER / LOT_SIZE rounding
// can happen client-side. Weight is 10 on Spot.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Exchange_Info, String_View>
fetch_exchange_info(S& stream, String_View host, Rate_Limiter& limiter,
                    i64 now_ms_value) noexcept {
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = "/api/v3/exchangeInfo"_v;
    req.host = host;
    constexpr i64 weight = 10;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Exchange_Info, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Exchange_Info>(fetched.unwrap().response);
}

// GET /api/v3/ticker/bookTicker — best bid/ask price and qty.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Book_Ticker, String_View>
fetch_book_ticker(S& stream, String_View host, Rate_Limiter& limiter, i64 now_ms_value,
                  String_View symbol) noexcept {
    Exchange::Binance::Query_Builder<Mdefault> q;
    q.add("symbol"_v, symbol);
    auto path_root = "/api/v3/ticker/bookTicker?"_v.append<Mdefault>(q.view());
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = path_root.view();
    req.host = host;

    constexpr i64 weight = 2;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if (!fetched.ok()) {
        return Result<Book_Ticker, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Book_Ticker>(fetched.unwrap().response);
}

} // namespace spp::App::Binance
