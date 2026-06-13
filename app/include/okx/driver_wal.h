#pragma once

// Binary append-only WAL for the OKX live driver's volatile state.
//
// Problem: the driver's `dispatch_cursor_`, `exchange_ord_id_`, and
// `cancel_done_` are all in-process maps.  A restart (crash, OOM kill,
// deliberate deploy) wipes them, and any in-flight order becomes an
// orphan — the exchange still has it open but the local state doesn't
// know how to cancel or correlate it with the strategy's order_id.
//
// This WAL records every state mutation as it happens.  On startup the
// caller replays it into the driver, restoring the mapping without any
// exchange round-trip.
//
// Binary record format (compact, no reflection, no dependency):
//
//   [1B type] [varint-u16 payload_len] [payload bytes]
//
//   Type 0x01  order_submitted
//     payload: <2B len><order_id bytes>
//              <2B len><code bytes>
//              <1B direction> <1B offset>
//              <8B price_raw i64 big-endian>
//              <8B volume f64 IEEE-754 big-endian>
//   Type 0x02  order_confirmed
//     payload: <2B len><order_id bytes>
//              <2B len><exchange_ordId bytes>
//   Type 0x03  order_cancelled
//     payload: <2B len><order_id bytes>
//   Type 0xFF  checkpoint
//     payload: <8B counter i64 big-endian>
//
// All multi-byte integers are big-endian (network byte order) so
// dumps are readable in xxd / hexfiend without flipping.

#include <spp/core/base.h>
#include <spp/containers/map.h>
#include <spp/containers/string1.h>
#include <spp/containers/vec.h>
#include <spp/io/wal.h>
#include <spp/quant/strategy/types.h>

namespace spp::App::Okx {

// Each of these functions writes a single WAL record.  All three are
// callable without constructing any intermediate object — just pass the
// writer the driver already holds.

[[nodiscard]] inline Result<u64, String_View>
wal_append_order(WAL::Writer& writer, String_View order_id, String_View code,
                 spp::quant::strategy::Order_Direction dir,
                 spp::quant::strategy::Order_Offset offset,
                 i64 price_raw, f64 volume) noexcept {
    u64 orlen = order_id.length();
    u64 clen  = code.length();
    if(orlen > 65535 || clen > 65535) return Result<u64, String_View>::err("wal_field_too_long"_v);
    u64 plen = 2 + orlen + 2 + clen + 1 + 1 + 8 + 8;

    u8 buf[512];
    u64 p = 0;
    buf[p++] = 0x01;
    if(plen < 128) { buf[p++] = static_cast<u8>(plen); }
    else           { buf[p++] = static_cast<u8>((plen >> 8) | 0x80); buf[p++] = static_cast<u8>(plen & 0xff); }
    buf[p++] = static_cast<u8>(orlen >> 8); buf[p++] = static_cast<u8>(orlen & 0xff);
    for(u64 i = 0; i < orlen; i++) buf[p++] = order_id[i];
    buf[p++] = static_cast<u8>(clen >> 8); buf[p++] = static_cast<u8>(clen & 0xff);
    for(u64 i = 0; i < clen; i++) buf[p++] = code[i];
    buf[p++] = static_cast<u8>(dir);
    buf[p++] = static_cast<u8>(offset);
    u64 pr = static_cast<u64>(price_raw);
    for(i32 i = 7; i >= 0; i--) buf[p++] = static_cast<u8>((pr >> (8 * i)) & 0xff);
    u64 vbits = *reinterpret_cast<const u64*>(&volume);
    for(i32 i = 7; i >= 0; i--) buf[p++] = static_cast<u8>((vbits >> (8 * i)) & 0xff);

    return writer.append_result(Slice<const u8>{buf, p});
}

[[nodiscard]] inline Result<u64, String_View>
wal_append_confirm(WAL::Writer& writer, String_View order_id,
                   String_View exchange_ordId) noexcept {
    u64 orlen = order_id.length();
    u64 exlen = exchange_ordId.length();
    if(orlen > 65535 || exlen > 65535) return Result<u64, String_View>::err("wal_field_too_long"_v);
    u64 plen = 2 + orlen + 2 + exlen;

    u8 buf[512];
    u64 p = 0;
    buf[p++] = 0x02;
    if(plen < 128) buf[p++] = static_cast<u8>(plen);
    else           { buf[p++] = static_cast<u8>((plen >> 8) | 0x80); buf[p++] = static_cast<u8>(plen & 0xff); }
    buf[p++] = static_cast<u8>(orlen >> 8); buf[p++] = static_cast<u8>(orlen & 0xff);
    for(u64 i = 0; i < orlen; i++) buf[p++] = order_id[i];
    buf[p++] = static_cast<u8>(exlen >> 8); buf[p++] = static_cast<u8>(exlen & 0xff);
    for(u64 i = 0; i < exlen; i++) buf[p++] = exchange_ordId[i];

    return writer.append_result(Slice<const u8>{buf, p});
}

[[nodiscard]] inline Result<u64, String_View>
wal_append_cancel(WAL::Writer& writer, String_View order_id) noexcept {
    u64 orlen = order_id.length();
    if(orlen > 65535) return Result<u64, String_View>::err("wal_field_too_long"_v);
    u64 plen = 2 + orlen;

    u8 buf[512];
    u64 p = 0;
    buf[p++] = 0x03;
    if(plen < 128) buf[p++] = static_cast<u8>(plen);
    else           { buf[p++] = static_cast<u8>((plen >> 8) | 0x80); buf[p++] = static_cast<u8>(plen & 0xff); }
    buf[p++] = static_cast<u8>(orlen >> 8); buf[p++] = static_cast<u8>(orlen & 0xff);
    for(u64 i = 0; i < orlen; i++) buf[p++] = order_id[i];

    return writer.append_result(Slice<const u8>{buf, p});
}

// Replay helper: reads WAL bytes back into the driver state.  Returns
// two maps that can be merged into the driver:
//
//   exchange_ord_id_:  order_id → exchange ordId (type 0x02)
//   cancel_done_:      order_id → 1         (type 0x03)
//
// Type 0x01 (order_submitted) entries are counted but not stored — they
// inform the caller how many pending orders existed at crash time (the
// caller can retrieve those by checking acc.orders between the
// reconstructed cursor and the end of the order log).
//
// Type 0xFF (checkpoint) entries signal "everything before me was
// written to disk and confirmed" — useful when GC'ing old WAL segments
// but not consumed here.

namespace wal_detail {

// Decode a big-endian u16 from bytes[pos].  Advances pos by 2.
[[nodiscard]] inline u64 read_u16_(Slice<const u8> buf, u64& pos) noexcept {
    if(pos + 2 > buf.length()) { pos = buf.length(); return 0; }
    u64 v = (static_cast<u64>(buf[pos]) << 8) | buf[pos + 1];
    pos += 2;
    return v;
}

// Decode a big-endian i64 from bytes[pos].  Advances pos by 8.
[[nodiscard]] inline i64 read_i64_(Slice<const u8> buf, u64& pos) noexcept {
    if(pos + 8 > buf.length()) { pos = buf.length(); return 0; }
    u64 v = 0;
    for(u64 i = 0; i < 8; i++) v = (v << 8) | buf[pos + i];
    pos += 8;
    return static_cast<i64>(v);
}

template<Allocator A>
inline void replay_entry_(u8 type, Slice<const u8> payload,
                           Map<String<A>, String<A>, A>& ord_ids,
                           Map<String<A>, u8, A>& cancel_set) noexcept {
    u64 pos = 0;
    if(type == 0x02 && payload.length() >= 5) {
        u64 orlen = read_u16_(payload, pos);
        if(pos + orlen > payload.length()) return;
        String_View oid{payload.data() + pos, orlen};
        pos += orlen;
        u64 exlen = read_u16_(payload, pos);
        if(pos + exlen > payload.length()) return;
        String_View ex{payload.data() + pos, exlen};
        // Dedup: over-write a prior mapping for the same key so
        // length() reflects the unique key count, not insert count.
        auto prev = ord_ids.try_get(oid);
        if(prev.ok()) {
            **prev = ex.template string<A>();
        } else {
            ord_ids.insert(oid.template string<A>(),
                           ex.template string<A>());
        }
    } else if(type == 0x03 && payload.length() >= 3) {
        u64 orlen = read_u16_(payload, pos);
        if(pos + orlen > payload.length()) return;
        String_View oid{payload.data() + pos, orlen};
        if(!cancel_set.contains(oid)) {
            cancel_set.insert(oid.template string<A>(), static_cast<u8>(1));
        }
    }
}

} // namespace wal_detail

// Replay on crash recovery.
template<Allocator A = Mdefault>
struct WAL_State {
    Map<String<A>, String<A>, A> exchange_ord_id;
    Map<String<A>, u8, A> cancel_done;
    // Count of type-0x01 entries — number of pending orders submitted
    // but not yet confirmed/cancelled at crash time.
    u64 pending_at_crash = 0;
};

template<Allocator A = Mdefault>
[[nodiscard]] inline WAL_State<A>
replay_wal(Slice<const u8> raw_wal_bytes) noexcept {
    WAL_State<A> state;
    u64 pos = 0;
    while(pos < raw_wal_bytes.length()) {
        u8 type = raw_wal_bytes[pos++];
        if(pos >= raw_wal_bytes.length()) break;

        u64 plen = raw_wal_bytes[pos++];
        if(plen >= 128) {
            // Two-byte varint: hi 7 bits of first byte + full second byte.
            if(pos >= raw_wal_bytes.length()) break;
            plen = ((plen & 0x7f) << 8) | raw_wal_bytes[pos++];
        }
        if(pos + plen > raw_wal_bytes.length()) break;

        Slice<const u8> payload{raw_wal_bytes.data() + pos, plen};
        pos += plen;

        if(type == 0x01) {
            state.pending_at_crash++;
        } else if(type == 0x02 || type == 0x03) {
            wal_detail::replay_entry_<A>(type, payload,
                                          state.exchange_ord_id,
                                          state.cancel_done);
        }
    }
    return state;
}

} // namespace spp::App::Okx
