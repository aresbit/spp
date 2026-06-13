#pragma once

// =============================================================================
// 10 message structs from the exam spec + a 152-byte type-erased envelope.
// No <cstring> dependency: we use __builtin_memcpy directly.
// =============================================================================

#include <spp/concurrency/spsc_ring.h>

namespace spp {
namespace Bench {
namespace Spsc {

using ::spp::Concurrency::u8;
using ::spp::Concurrency::u32;
using ::spp::Concurrency::u64;
using ::spp::Concurrency::i64;

struct Msg1  { char data[80];  };
struct Msg2  { char data[120]; };
struct Msg3  { char data[96];  };
struct Msg4  { char data[128]; };
struct Msg5  { char data[64];  };
struct Msg6  { char data[140]; };
struct Msg7  { char data[88];  };
struct Msg8  { char data[104]; };
struct Msg9  { char data[72];  };
struct Msg10 { char data[132]; };

static constexpr u64 MAX_MSG_PAYLOAD = 140;  // Msg6

enum class MsgType : u32 {
    Msg1 = 0, Msg2, Msg3, Msg4, Msg5,
    Msg6, Msg7, Msg8, Msg9, Msg10, COUNT
};

inline constexpr u64 msg_payload_size(MsgType t) noexcept {
    constexpr u64 sizes[] = {80, 120, 96, 128, 64, 140, 88, 104, 72, 132};
    auto i = static_cast<u32>(t);
    return i < 10 ? sizes[i] : 0;
}

// =============================================================================
// 152-byte envelope: tag (4) + pad (4) + payload (140) + alignment slack.
// Trivially copyable, single memcpy on hot path.
// =============================================================================
struct alignas(8) MsgEnvelope {
    MsgType  type;
    u32      pad;
    char     data[MAX_MSG_PAYLOAD];

    MsgEnvelope() noexcept : type(MsgType::Msg1), pad(0) {}
};
static_assert(sizeof(MsgEnvelope) == 152,
              "MsgEnvelope must be 152 bytes");

// =============================================================================
// Trivial xorshift PRNG for deterministic random payloads.
// =============================================================================
inline u32 xorshift32(u32 s) noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline void fill_random(char* buf, u64 n, u32 seed) noexcept {
    for(u64 i = 0; i < n; ++i) {
        seed = xorshift32(seed);
        buf[i] = static_cast<char>(seed & 0xff);
    }
}

inline void generate_messages(MsgEnvelope* out, u64 count,
                              u32 seed = 0x12345u) noexcept {
    for(u64 i = 0; i < count; ++i) {
        u32 idx = seed % 10;
        auto t  = static_cast<MsgType>(idx);
        out[i].type = t;
        out[i].pad  = 0;
        fill_random(out[i].data, msg_payload_size(t), seed);
        seed = xorshift32(seed);
    }
}

// Force the optimiser to keep memory reads visible.
inline u64 checksum_envelope(const MsgEnvelope& e, u64 acc) noexcept {
    const char* p = e.data;
    u64 n = msg_payload_size(e.type);
    for(u64 i = 0; i < n; ++i) acc = (acc * 31u) ^ static_cast<u8>(p[i]);
    acc ^= static_cast<u64>(e.type);
    return acc;
}

} // namespace Spsc
} // namespace Bench
} // namespace spp
