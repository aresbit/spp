#pragma once

// Time helpers for OKX. OKX wants two different timestamp formats:
//
//   1. REST signing: ISO 8601 with millisecond precision and a trailing Z,
//      e.g. "2020-12-08T09:08:57.715Z".  This is the literal value of the
//      OK-ACCESS-TIMESTAMP header AND the leading prefix in the prehash
//      string.
//
//   2. WS private-channel login: UNIX seconds as a decimal string, e.g.
//      "1597026383".  No milliseconds, no separators.
//
// Both derive from the same `now_ms` source (gettimeofday-backed in the
// Binance helper we share). Format helpers return owned `String<Mdefault>`
// so callers can hold the buffer alive for the duration of the request.

#include <spp/core/base.h>
#include <spp/containers/string1.h>
#include <binance/clock.h>   // now_ms()

#include <time.h>

namespace spp::App::Okx {

using App::Binance::now_ms;

// "YYYY-MM-DDTHH:MM:SS.sssZ" — exactly 24 chars + NUL.
[[nodiscard]] inline String<Mdefault> iso8601_ms(i64 unix_ms) noexcept {
    time_t sec = static_cast<time_t>(unix_ms / 1000);
    i64 ms = unix_ms - static_cast<i64>(sec) * 1000;
    if(ms < 0) { ms += 1000; sec -= 1; }   // mirror divmod-with-truncation
    struct tm tm_buf;
    static_cast<void>(gmtime_r(&sec, &tm_buf));

    char buf[40];
    i32 n = Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf),
                           "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                           tm_buf.tm_year + 1900,
                           tm_buf.tm_mon + 1,
                           tm_buf.tm_mday,
                           tm_buf.tm_hour,
                           tm_buf.tm_min,
                           tm_buf.tm_sec,
                           static_cast<long long>(ms));
    String<Mdefault> out(static_cast<u64>(n));
    out.set_length(static_cast<u64>(n));
    Libc::memcpy(out.data(), buf, static_cast<u64>(n));
    return out;
}

// Plain UNIX seconds, decimal string.  Used by the WS private-channel
// `login` op.
[[nodiscard]] inline String<Mdefault> unix_sec_str(i64 unix_ms) noexcept {
    char buf[24];
    i32 n = Libc::snprintf(reinterpret_cast<u8*>(buf), sizeof(buf),
                           "%lld", static_cast<long long>(unix_ms / 1000));
    String<Mdefault> out(static_cast<u64>(n));
    out.set_length(static_cast<u64>(n));
    Libc::memcpy(out.data(), buf, static_cast<u64>(n));
    return out;
}

} // namespace spp::App::Okx
