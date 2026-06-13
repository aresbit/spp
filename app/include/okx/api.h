#pragma once

// OKX V5 REST helpers.
//
// Shape mirrors the Binance helpers:
//   - templated on a Byte_Stream so callers pick TLS / plain TCP / memory
//   - explicit `now_ms_value` for deterministic tests
//   - `Rate_Limiter` parameter so the signer flows through one shared
//     throttle
//   - typed response parsed via SPP reflection
//
// All POST bodies are JSON.  We serialise `Order_Request` manually so
// optional fields (px when ordType=market, clOrdId when empty) don't
// emit empty-string keys that OKX would reject.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/io/net.h>
#include <spp/reflection/json.h>
#include <spp/quant/data/types.h>

#include <binance/filter_round.h>

#include <okx/models.h>
#include <okx/rate_limiter.h>
#include <okx/signer.h>

namespace spp::App::Okx {

namespace detail {

// JSON-escape minimum: ", \, \n.  OKX field values come from the caller
// (api_key / passphrase get put in headers, never bodies) so the threat
// model is limited to numeric strings that happen to contain a quote —
// unlikely but cheap to guard.
inline void json_quoted(Vec<u8, Mdefault>& out, String_View v) noexcept {
    out.push('"');
    for(u8 c : v) {
        if(c == '"' || c == '\\') out.push('\\');
        if(c == '\n') { out.push('\\'); out.push('n'); continue; }
        out.push(c);
    }
    out.push('"');
}

template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] Result<Protocol::Http::Fetched_Response<Mdefault>, String_View>
send_throttled(S& stream, const Protocol::Http::Request<Mdefault>& req,
               Rate_Limiter& limiter, i64 weight, i64 now_ms) noexcept {
    i64 wait = limiter.wait_required_ms(weight, now_ms);
    if(wait > 0) {
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

// Serialise an Order_Request into the canonical JSON body OKX expects.
// Optional fields are emitted only when set, since OKX rejects empty-
// string values for `px` (-> "Order price must be greater than 0").
[[nodiscard]] inline String<Mdefault>
build_order_body(const Order_Request& o) noexcept {
    Vec<u8, Mdefault> buf;
    auto put_lit = [&buf](const char* s) {
        while(*s) buf.push(static_cast<u8>(*s++));
    };
    buf.push('{');
    put_lit("\"instId\":"); json_quoted(buf, o.instId);
    put_lit(",\"tdMode\":"); json_quoted(buf, wire_name(o.tdMode));
    put_lit(",\"side\":"); json_quoted(buf, wire_name(o.side));
    put_lit(",\"ordType\":"); json_quoted(buf, wire_name(o.ordType));
    if(o.sz.length() > 0) {
        put_lit(",\"sz\":"); json_quoted(buf, o.sz);
    }
    if(o.px.length() > 0) {
        put_lit(",\"px\":"); json_quoted(buf, o.px);
    }
    if(o.clOrdId.length() > 0) {
        put_lit(",\"clOrdId\":"); json_quoted(buf, o.clOrdId);
    }
    buf.push('}');

    String<Mdefault> out(buf.length());
    out.set_length(buf.length());
    if(buf.length() > 0) Libc::memcpy(out.data(), buf.data(), buf.length());
    return out;
}

} // namespace detail

// GET /api/v5/public/time — clock skew / connectivity probe.  No auth.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Server_Time_Resp, String_View>
fetch_server_time(S& stream, String_View host, Rate_Limiter& limiter,
                  i64 now_ms_value) noexcept {
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = "/api/v5/public/time"_v;
    req.host = host;
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Server_Time_Resp, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Server_Time_Resp>(fetched.unwrap().response);
}

// GET /api/v5/market/ticker?instId=<sym>
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Ticker_Resp, String_View>
fetch_ticker(S& stream, String_View host, Rate_Limiter& limiter,
             i64 now_ms_value, String_View instId) noexcept {
    auto query = "instId="_v.append<Mdefault>(instId);
    auto path = "/api/v5/market/ticker?"_v.append<Mdefault>(query.view());
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = path.view();
    req.host = host;
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Ticker_Resp, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Ticker_Resp>(fetched.unwrap().response);
}

// GET /api/v5/public/instruments?instType=SPOT — public, no auth.
// Populate the Filter_Cache from this on startup so order prices and
// quantities are automatically rounded to tickSize / lotSize.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Instruments_Resp, String_View>
fetch_instruments(S& stream, String_View host, Rate_Limiter& limiter,
                  i64 now_ms_value, String_View instType = "SPOT"_v) noexcept {
    auto query = "instType="_v.append<Mdefault>(instType);
    auto path = "/api/v5/public/instruments?"_v.append<Mdefault>(query.view());
    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = path.view();
    req.host = host;
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Instruments_Resp, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Instruments_Resp>(fetched.unwrap().response);
}

// GET /api/v5/account/balance — SIGNED.  Optionally narrowed to one ccy.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Balance_Resp, String_View>
fetch_balance(S& stream, String_View host, const Signer_Config& cfg,
              Rate_Limiter& limiter, i64 now_ms_value,
              String_View ccy = ""_v) noexcept {
    String<Mdefault> path_owned;
    if(ccy.length() > 0) {
        auto q = "ccy="_v.append<Mdefault>(ccy);
        path_owned = "/api/v5/account/balance?"_v.append<Mdefault>(q.view());
    } else {
        path_owned = "/api/v5/account/balance"_v.string<Mdefault>();
    }
    auto sr = signed_request_full<Mdefault>(
        "GET"_v, path_owned.view(), ""_v, ""_v, host, cfg, now_ms_value);
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, sr.request, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Balance_Resp, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Balance_Resp>(fetched.unwrap().response);
}

// POST /api/v5/trade/order — SIGNED.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Place_Order_Resp, String_View>
place_order(S& stream, String_View host, const Signer_Config& cfg,
            Rate_Limiter& limiter, i64 now_ms_value,
            const Order_Request& order) noexcept {
    auto body = detail::build_order_body(order);
    auto sr = signed_request_full<Mdefault>(
        "POST"_v, "/api/v5/trade/order"_v, ""_v, body.view(),
        host, cfg, now_ms_value);
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, sr.request, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Place_Order_Resp, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Place_Order_Resp>(fetched.unwrap().response);
}

// POST /api/v5/trade/cancel-order — SIGNED.  Address an order by
// `ordId` (exchange-side) OR `clOrdId` (caller-side); supplying both is
// redundant — OKX trusts ordId if it's present.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Cancel_Order_Resp, String_View>
cancel_order(S& stream, String_View host, const Signer_Config& cfg,
             Rate_Limiter& limiter, i64 now_ms_value,
             String_View instId, String_View ordId,
             String_View clOrdId = ""_v) noexcept {
    Vec<u8, Mdefault> buf;
    auto put_lit = [&buf](const char* s) {
        while(*s) buf.push(static_cast<u8>(*s++));
    };
    buf.push('{');
    put_lit("\"instId\":"); detail::json_quoted(buf, instId);
    if(ordId.length() > 0) {
        put_lit(",\"ordId\":"); detail::json_quoted(buf, ordId);
    }
    if(clOrdId.length() > 0) {
        put_lit(",\"clOrdId\":"); detail::json_quoted(buf, clOrdId);
    }
    buf.push('}');
    String<Mdefault> body(buf.length());
    body.set_length(buf.length());
    Libc::memcpy(body.data(), buf.data(), buf.length());

    auto sr = signed_request_full<Mdefault>(
        "POST"_v, "/api/v5/trade/cancel-order"_v, ""_v, body.view(),
        host, cfg, now_ms_value);
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, sr.request, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Cancel_Order_Resp, String_View>::err(spp::move(fetched.unwrap_err()));
    }
    return detail::parse_body<Cancel_Order_Resp>(fetched.unwrap().response);
}

// Manually parse `{"code":"0","msg":"","data":[[...]]}`.  We tolerate
// extra fields and stop reading once the matching `]` of `data` is
// reached.  Each row is exactly the OKX kline shape; missing trailing
// columns are left as defaults.
[[nodiscard]] inline Result<Vec<Candlestick, Mdefault>, String_View>
parse_candles(String_View body) noexcept {
    // Locate "data":[ — OKX always returns it as the third field but we
    // don't want to bake field order in.
    u64 i = 0;
    bool found = false;
    while(i + 7 < body.length()) {
        if(body[i] == '"' && body[i + 1] == 'd' && body[i + 2] == 'a' &&
           body[i + 3] == 't' && body[i + 4] == 'a' && body[i + 5] == '"') {
            i += 6;
            while(i < body.length() && (body[i] == ' ' || body[i] == ':' ||
                                         body[i] == '\t' || body[i] == '\n')) {
                i++;
            }
            if(i < body.length() && body[i] == '[') { found = true; break; }
        }
        i++;
    }
    if(!found) return Result<Vec<Candlestick, Mdefault>, String_View>::err(
        "candles_no_data_array"_v);

    Vec<Candlestick, Mdefault> out;
    i++; // past first '['

    // Walk rows: each row is a `[...]` array of strings/numbers.
    while(i < body.length()) {
        while(i < body.length() && (body[i] == ' ' || body[i] == ',' ||
                                     body[i] == '\n' || body[i] == '\t')) {
            i++;
        }
        if(i >= body.length() || body[i] == ']') break;
        if(body[i] != '[') {
            return Result<Vec<Candlestick, Mdefault>, String_View>::err(
                "candles_row_not_array"_v);
        }
        i++; // past '['

        Candlestick c;
        u64 col = 0;
        while(i < body.length() && body[i] != ']') {
            while(i < body.length() && (body[i] == ' ' || body[i] == ',' ||
                                         body[i] == '\t')) {
                i++;
            }
            if(i >= body.length() || body[i] == ']') break;
            // A field is either "string" or bareword (no quotes).
            String_View field;
            if(body[i] == '"') {
                u64 start = ++i;
                while(i < body.length() && body[i] != '"') i++;
                field = String_View{body.data() + start, i - start};
                if(i < body.length()) i++;
            } else {
                u64 start = i;
                while(i < body.length() && body[i] != ',' && body[i] != ']' &&
                      body[i] != ' ' && body[i] != '\t') {
                    i++;
                }
                field = String_View{body.data() + start, i - start};
            }
            // Map by column index — see Candlestick docstring for layout.
            switch(col) {
            case 0: {
                i64 v = 0;
                for(u8 ch : field) {
                    if(ch >= '0' && ch <= '9') v = v * 10 + (ch - '0');
                }
                c.open_time = v;
                break;
            }
            case 1: c.open = field.string<Mdefault>(); break;
            case 2: c.high = field.string<Mdefault>(); break;
            case 3: c.low = field.string<Mdefault>(); break;
            case 4: c.close = field.string<Mdefault>(); break;
            case 5: c.vol = field.string<Mdefault>(); break;
            case 6: c.volCcy = field.string<Mdefault>(); break;
            case 7: c.volCcyQuote = field.string<Mdefault>(); break;
            case 8: {
                i32 v = 0;
                for(u8 ch : field) {
                    if(ch >= '0' && ch <= '9') v = v * 10 + (ch - '0');
                }
                c.confirm = v;
                break;
            }
            default: break;
            }
            col++;
        }
        if(i < body.length() && body[i] == ']') i++;
        out.push(spp::move(c));
    }
    return Result<Vec<Candlestick, Mdefault>, String_View>::ok(spp::move(out));
}

// GET /api/v5/market/candles?instId=<sym>&bar=<freq>&limit=<n>.
// The response shape is `{"code":..,"data":[[ts, o, h, l, c, vol, ...]]}`
// — array of arrays — so we hand the body to `parse_candles` after the
// HTTP fetch completes.
template<typename S>
    requires Net::Byte_Stream<S>
[[nodiscard]] inline Result<Vec<Candlestick, Mdefault>, String_View>
fetch_candles(S& stream, String_View host, Rate_Limiter& limiter,
              i64 now_ms_value, String_View instId, String_View bar,
              u32 limit = 100) noexcept {
    char limit_buf[16];
    i32 ln = Libc::snprintf(reinterpret_cast<u8*>(limit_buf), sizeof(limit_buf),
                            "%u", limit);
    String_View limit_sv{reinterpret_cast<const u8*>(limit_buf),
                         static_cast<u64>(ln > 0 ? ln : 1)};

    auto q1 = "instId="_v.append<Mdefault>(instId);
    auto q2 = q1.view().append<Mdefault>("&bar="_v);
    auto q3 = q2.view().append<Mdefault>(bar);
    auto q4 = q3.view().append<Mdefault>("&limit="_v);
    auto q  = q4.view().append<Mdefault>(limit_sv);
    auto path = "/api/v5/market/candles?"_v.append<Mdefault>(q.view());

    Protocol::Http::Request<Mdefault> req;
    req.method = "GET"_v;
    req.path = path.view();
    req.host = host;
    constexpr i64 weight = 1;
    auto fetched = detail::send_throttled(stream, req, limiter, weight, now_ms_value);
    if(!fetched.ok()) {
        return Result<Vec<Candlestick, Mdefault>, String_View>::err(
            spp::move(fetched.unwrap_err()));
    }
    String_View body{fetched.unwrap().response.body.data(),
                     fetched.unwrap().response.body.length()};
    return parse_candles(body);
}

// Populate a Filter_Cache from an OKX /api/v5/public/instruments
// response.  One map entry per instrument; the cache can then round
// prices to tickSz, quantities to lotSz/minSz, and enforce minSz.
// Call this at startup before the main loop.
template<typename A>
inline void populate_filter_cache_from_instruments(
    Binance::Filter_Cache<A>& cache,
    const Instruments_Resp& resp) noexcept {
    auto str_to_f64 = [](String_View sv) noexcept -> f64 {
        f64 whole = 0.0, frac = 0.0, div = 1.0;
        bool neg = false;
        u64 i = 0;
        if(i < sv.length() && sv[i] == '-') { neg = true; i++; }
        while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
            whole = whole * 10.0 + static_cast<f64>(sv[i] - '0'); i++;
        }
        if(i < sv.length() && sv[i] == '.') {
            i++;
            while(i < sv.length() && sv[i] >= '0' && sv[i] <= '9') {
                frac = frac * 10.0 + static_cast<f64>(sv[i] - '0');
                div *= 10.0; i++;
            }
        }
        return neg ? -(whole + frac / div) : (whole + frac / div);
    };
    for(u64 i = 0; i < resp.data.length(); i++) {
        const auto& inst = resp.data[i];
        Binance::Symbol_Filters sf;
        sf.tick_size = str_to_f64(inst.tickSz.view());
        sf.step_size = str_to_f64(inst.lotSz.view());
        sf.min_qty   = str_to_f64(inst.minSz.view());
        sf.max_qty   = 0.0;
        sf.min_notional = 0.0;
        cache.put(inst.instId.view(), sf);
    }
}

} // namespace spp::App::Okx
