#pragma once

// Event-loop multiplexer for the OKX live driver.
//
// Without this, the production main loop alternates blocking pumps:
//
//     drv.pump_market_once(now_ms);   // blocks if market is idle
//     drv.pump_user_once(user);       // can't service user fills
//
// During a quiet minute the market recv may sit for 30+ seconds while a
// fill event sits unprocessed in the user-stream's socket buffer.  The
// multiplexer waits on BOTH sockets simultaneously and dispatches
// whichever is ready first.
//
// TLS gotcha: mbedTLS keeps decrypted application bytes in its session
// buffer between calls.  A `recv_result` may consume only some of those
// bytes; the remainder won't trigger a kernel-level POLLIN even though
// they're waiting for the next `recv_result`.  We check
// `pending_app_bytes()` on every source BEFORE polling — any source
// with buffered bytes is treated as immediately ready.
//
// The helper is templated on the two underlying TLS stream types so it
// works for tests against any `Byte_Stream` whose handle / pending
// accessors compile away to no-ops when irrelevant.

#include <spp/core/base.h>
#include <spp/core/result.h>
#include <spp/io/handle.h>

namespace spp::App::Okx {

// Source readiness flags returned by `poll_any`.
enum class Source_Ready : u8 {
    none    = 0,   // timeout — neither stream was ready
    market  = 1,
    user    = 2,
    both    = 3,
};

[[nodiscard]] constexpr bool ready_market(Source_Ready r) noexcept {
    return static_cast<u8>(r) & 1u;
}
[[nodiscard]] constexpr bool ready_user(Source_Ready r) noexcept {
    return static_cast<u8>(r) & 2u;
}

// Concept: any stream that exposes pending TLS bytes + native fd.
// `Net::Memory_Stream` does NOT satisfy this — the multiplexer is a
// production-path helper, not a unit-test one. Tests drive
// `pump_market_once` / `pump_user_once` synchronously.
template<typename S>
concept Has_Pollable_Native = requires(S& s) {
    { s.pending_app_bytes() } -> Same<u64>;
    { s.native_handle() } -> Same<const IO::Handle&>;
};

// Wait up to `timeout_ms` for either `market_tls` or `user_tls` to
// have readable bytes.  Returns:
//   - Source_Ready::market / user / both — at least one is ready
//   - Source_Ready::none                — timeout elapsed cleanly
//   - err                                — IO error from poll syscall
template<typename Market_Tls, typename User_Tls>
    requires Has_Pollable_Native<Market_Tls> && Has_Pollable_Native<User_Tls>
[[nodiscard]] inline Result<Source_Ready, String_View>
poll_either(Market_Tls& market_tls, User_Tls& user_tls,
            u64 timeout_ms) noexcept {
    // Step 1: TLS-buffered data trumps the kernel.  If either side has
    // decrypted bytes waiting, return immediately — polling would only
    // tell us about NEW socket bytes, missing what's already inside the
    // TLS layer.
    u8 ready_bits = 0;
    if(market_tls.pending_app_bytes() > 0) ready_bits |= 1u;
    if(user_tls.pending_app_bytes()   > 0) ready_bits |= 2u;
    if(ready_bits != 0) {
        return Result<Source_Ready, String_View>::ok(
            static_cast<Source_Ready>(ready_bits));
    }

    // Step 2: poll the underlying fds.  IO::poll_any_result reports the
    // FIRST ready target by index but its `Poll_Target` carries
    // `revents` per slot, so we can detect both being ready in one call.
    IO::Handle market_h = market_tls.native_handle();
    IO::Handle user_h   = user_tls.native_handle();
    IO::Poll_Target targets[2];
    targets[0].handle = &market_h;
    targets[0].events = IO::Poll_Event::in;
    targets[1].handle = &user_h;
    targets[1].events = IO::Poll_Event::in;

    auto r = IO::poll_any_result(Slice<IO::Poll_Target>{targets, 2}, timeout_ms);
    if(!r.ok()) return Result<Source_Ready, String_View>::err("poll_failed"_v);

    // Translate revents into the bitmask. The poll syscall fills every
    // target's revents independently — both can be set in one wakeup.
    if(IO::has_any(targets[0].revents,
                    IO::Poll_Event::in | IO::Poll_Event::hup |
                    IO::Poll_Event::err)) {
        ready_bits |= 1u;
    }
    if(IO::has_any(targets[1].revents,
                    IO::Poll_Event::in | IO::Poll_Event::hup |
                    IO::Poll_Event::err)) {
        ready_bits |= 2u;
    }
    return Result<Source_Ready, String_View>::ok(
        static_cast<Source_Ready>(ready_bits));
}

} // namespace spp::App::Okx
