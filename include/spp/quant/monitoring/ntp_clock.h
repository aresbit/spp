#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/base/date.h"

#ifdef SPP_OS_LINUX
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>
#include <cstring>
#endif

namespace spp::quant::monitoring {

// =========================================================================
// NTPClock — NTP time synchronization for microsecond-accurate timestamps
//
// Uses SNTP (Simple NTP) protocol per RFC 5905. Queries NTP servers
// periodically and maintains a clock offset correction. All timestamps
// from get_time() are corrected by the estimated offset.
//
// NTP packet format (RFC 5905 Section 7.3): 48 bytes, UDP port 123
//
// Timestamp encoding:
//   64-bit fixed-point: 32-bit integer seconds since 1900-01-01 00:00:00 UTC
//                     + 32-bit fraction of a second (units of 2^-32 s)
//
// Conversion to Unix epoch:
//   epoch_offset = 2208988800 (seconds between 1900-01-01 and 1970-01-01)
//   unix_us = (ntp_sec - epoch_offset) * 1e6 + ntp_frac * 1e6 / 2^32
// =========================================================================

// NTP epoch offset: 1900-01-01 to 1970-01-01 in seconds
constexpr u64 NTP_EPOCH_OFFSET_SEC = 2208988800ULL;

// NTP packet indices (RFC 5905 Section 7.3)
constexpr u64 NTP_PACKET_SIZE        = 48;
constexpr u64 NTP_LI_VN_MODE_OFFSET  = 0;
constexpr u64 NTP_STRATUM_OFFSET     = 1;
constexpr u64 NTP_POLL_OFFSET        = 2;
constexpr u64 NTP_PRECISION_OFFSET   = 3;
constexpr u64 NTP_ROOT_DELAY_OFFSET  = 4;
constexpr u64 NTP_ROOT_DISP_OFFSET   = 8;
constexpr u64 NTP_REF_ID_OFFSET      = 12;
constexpr u64 NTP_REF_TS_OFFSET      = 16;
constexpr u64 NTP_ORIG_TS_OFFSET     = 24;
constexpr u64 NTP_RECV_TS_OFFSET     = 32;
constexpr u64 NTP_XMIT_TS_OFFSET     = 40;

// NTP modes
constexpr u8 NTP_MODE_CLIENT = 3;
constexpr u8 NTP_MODE_SERVER = 4;
// LI=0, VN=4, Mode encoded in lowest 3 bits
constexpr u8 NTP_CLIENT_HEADER = (0 << 6) | (4 << 3) | NTP_MODE_CLIENT; // 0x1B = 27

// =========================================================================
// NTPClock — NTP time synchronization
// =========================================================================

struct NTPClock {
    // NTP server configuration
    Vec<String_View> servers_;      // e.g., {"pool.ntp.org", "time.google.com"}
    u64  sync_interval_sec_  = 60;  // re-sync every 60 seconds
    i64  offset_us_          = 0;   // estimated offset: system_time + offset = ntp_time
    bool synchronized_       = false;
    f64  round_trip_ms_      = 0;
    u64  last_sync_timestamp_ = 0;

    // Create with default NTP servers
    static NTPClock create(Vec<String_View> servers = {}) noexcept {
        NTPClock clock;
        if (servers.empty()) {
            clock.servers_.push("pool.ntp.org"_v);
            clock.servers_.push("time.google.com"_v);
        } else {
            clock.servers_ = spp::move(servers);
        }
        return clock;
    }

    static NTPClock create_default() noexcept {
        return create({});
    }

    // =====================================================================
    // NTPResult — result from a single server query
    // =====================================================================
    struct NTPResult {
        i64 offset_us;       // correction to add to system clock (system + offset = ntp)
        f64 round_trip_ms;   // network round-trip time
        u64 server_time_us;  // server's time (microseconds since epoch)
    };

    // =====================================================================
    // query_server — query a single NTP server
    //
    // Returns the computed offset, RTT, and server time.
    // Returns None if the query fails (timeout, network error, invalid response).
    // =====================================================================
    static Opt<NTPResult> query_server(String_View server, u64 timeout_ms = 3000) noexcept {
#ifdef SPP_OS_LINUX
        // 1. Build NTP client request packet
        u8 packet[NTP_PACKET_SIZE] = {};
        build_ntp_packet(packet);

        // 2. Record send time T1
        u64 t1_us = system_time_us();

        // 3. Send UDP packet to server on port 123, receive response
        u8 recv_buf[NTP_PACKET_SIZE] = {};
        auto recv_opt = udp_send_recv(server, 123, packet, NTP_PACKET_SIZE,
                                       recv_buf, NTP_PACKET_SIZE, timeout_ms);
        if (!recv_opt.ok()) return {};

        // 4. Record receive time T4
        u64 t4_us = system_time_us();

        // 5. Parse NTP response
        return parse_ntp_response(recv_buf, t1_us, t4_us);
#else
        (void)server; (void)timeout_ms;
        return {};
#endif
    }

    // =====================================================================
    // sync — query all configured servers, take median offset
    //
    // Rejects outliers whose offset differs from the median by more than
    // MAX_OUTLIER_OFFSET_US (10ms).
    //
    // Updates offset_us_, round_trip_ms_, synchronized_ on success.
    // Returns true if at least one valid response was obtained.
    // =====================================================================
    bool sync() noexcept {
        Thread::Lock lock(mutex_);

        constexpr i64 MAX_OUTLIER_OFFSET_US = 10'000; // 10ms max deviation from median
        Vec<i64> offsets;
        Vec<f64> rtts;

        for (u64 i = 0; i < servers_.length(); i++) {
            auto result = query_server(servers_[i]);
            if (result.ok()) {
                offsets.push(result->offset_us);
                rtts.push(result->round_trip_ms);
            }
        }

        if (offsets.empty()) {
            synchronized_ = false;
            return false;
        }

        // Sort offsets to find median
        for (u64 i = 1; i < offsets.length(); i++) {
            i64 key = offsets[i];
            u64 j = i;
            while (j > 0 && offsets[j - 1] > key) {
                offsets[j] = offsets[j - 1];
                j--;
            }
            offsets[j] = key;
        }

        // Compute median offset
        i64 median_offset;
        u64 mid = offsets.length() / 2;
        if (offsets.length() % 2 == 0 && mid > 0) {
            median_offset = (offsets[mid - 1] + offsets[mid]) / 2;
        } else {
            median_offset = offsets[mid];
        }

        // Filter: keep offsets within MAX_OUTLIER_OFFSET_US of median,
        // then recompute as average of inliers for stability.
        i64 sum_offset = 0;
        f64 sum_rtt = 0.0;
        u64 inlier_count = 0;

        for (u64 i = 0; i < offsets.length(); i++) {
            i64 diff = offsets[i] - median_offset;
            if (diff < 0) diff = -diff;
            if (diff <= MAX_OUTLIER_OFFSET_US) {
                sum_offset += offsets[i];
                sum_rtt += rtts[i];
                inlier_count++;
            }
        }

        if (inlier_count == 0) {
            // All rejected — fall back to median of all
            offset_us_ = median_offset;
            round_trip_ms_ = rtts[mid]; // approximate
        } else {
            offset_us_ = sum_offset / static_cast<i64>(inlier_count);
            round_trip_ms_ = sum_rtt / static_cast<f64>(inlier_count);
        }

        synchronized_ = true;
        last_sync_timestamp_ = system_time_us();
        return true;
    }

    // =====================================================================
    // get_time_us — get current NTP-corrected time in microseconds since epoch
    // =====================================================================
    [[nodiscard]] u64 get_time_us() const noexcept {
        u64 sys_us = system_time_us();
        return system_to_ntp(sys_us);
    }

    // =====================================================================
    // get_date — get NTP-corrected Date
    // =====================================================================
    [[nodiscard]] Date get_date() const noexcept {
        // Convert NTP-corrected microseconds to days since epoch
        u64 ntp_us = get_time_us();
        u64 days = ntp_us / (86400000000ULL);
        return Date{static_cast<i32>(days)};
    }

    // =====================================================================
    // system_to_ntp — convert system time to NTP-corrected time
    // =====================================================================
    [[nodiscard]] u64 system_to_ntp(u64 system_time_us) const noexcept {
        i64 offset = offset_us_;
        if (offset >= 0) {
            return system_time_us + static_cast<u64>(offset);
        }
        i64 neg_offset = -offset;
        if (static_cast<u64>(neg_offset) > system_time_us) {
            return 0; // underflow protection
        }
        return system_time_us - static_cast<u64>(neg_offset);
    }

    // =====================================================================
    // is_synchronized — check if clock has been synchronized
    // =====================================================================
    [[nodiscard]] bool is_synchronized() const noexcept {
        return synchronized_;
    }

    // =====================================================================
    // offset_us — get current offset estimate
    // =====================================================================
    [[nodiscard]] i64 offset_us() const noexcept {
        return offset_us_;
    }

    // =====================================================================
    // accuracy_us — get clock accuracy estimate (from round-trip time)
    //
    // Accuracy is approximately half the RTT (assuming symmetric path).
    // =====================================================================
    [[nodiscard]] f64 accuracy_us() const noexcept {
        return round_trip_ms_ * 500.0; // half of RTT in microseconds
    }

    SPP_RECORD(NTPClock, SPP_FIELD(offset_us_), SPP_FIELD(synchronized_));

private:
    Thread::Mutex mutex_;

    // =====================================================================
    // system_time_us — get current system clock time in microseconds
    // =====================================================================
    [[nodiscard]] static u64 system_time_us() noexcept {
#ifdef SPP_OS_LINUX
        struct timeval tv;
        ::gettimeofday(&tv, null);
        return static_cast<u64>(tv.tv_sec) * 1'000'000ULL +
               static_cast<u64>(tv.tv_usec);
#else
        return 0;
#endif
    }

    // =====================================================================
    // build_ntp_packet — fill a 48-byte NTP client request packet
    //
    // RFC 5905 Section 7.3:
    //   LI=0 (no warning), VN=4 (version 4), Mode=3 (client)
    //   All timestamp fields are zero (server fills the transmit timestamp)
    // =====================================================================
    static void build_ntp_packet(u8 packet[NTP_PACKET_SIZE]) noexcept {
        // Zero-fill the entire packet
        for (u64 i = 0; i < NTP_PACKET_SIZE; i++) {
            packet[i] = 0;
        }

        // Byte 0: LI (bits 7-6) = 0, VN (bits 5-3) = 4, Mode (bits 2-0) = 3
        packet[NTP_LI_VN_MODE_OFFSET] = NTP_CLIENT_HEADER; // 0x1B

        // Optional: set the Transmit Timestamp to current time for symmetric
        // mode estimation. In SNTP client mode, we use T1 and T4 from the
        // system clock directly, so transmit timestamp can remain zero.
        // Some servers require this field to be populated, so we set it:
        //
        // Actually, per RFC 4330 (SNTPv4), the client SHOULD set the
        // Transmit Timestamp. Let's do it for maximum compatibility.
        u64 now_us = system_time_us();
        write_ntp_timestamp(packet, NTP_XMIT_TS_OFFSET, now_us);

        // Also copy to Originate Timestamp (Tag 24) so the server echoes it
        // back for the client to compute delay.
        // Wait — in RFC 5905, the Originate Timestamp (T1) is set by the
        // client to its departure time. We write the current time there too.
        write_ntp_timestamp(packet, NTP_ORIG_TS_OFFSET, now_us);
    }

    // =====================================================================
    // write_ntp_timestamp — write a 64-bit NTP timestamp into the buffer
    //
    // Converts microseconds-since-epoch to:
    //   seconds since 1900 (32-bit) + fraction in 2^-32 units (32-bit)
    // =====================================================================
    static void write_ntp_timestamp(u8 packet[NTP_PACKET_SIZE], u64 offset,
                                     u64 epoch_us) noexcept {
        // Convert to seconds since 1900
        u64 unix_sec = epoch_us / 1'000'000ULL;
        u64 unix_us_rem = epoch_us % 1'000'000ULL;
        u64 ntp_sec = unix_sec + NTP_EPOCH_OFFSET_SEC;

        // Convert microsecond remainder to NTP fraction (units of 2^-32)
        // fraction = (unix_us_rem * 2^32) / 1'000'000
        u64 ntp_frac = (unix_us_rem << 32) / 1'000'000ULL;

        // Write big-endian 32-bit seconds
        packet[offset + 0] = static_cast<u8>((ntp_sec >> 24) & 0xFF);
        packet[offset + 1] = static_cast<u8>((ntp_sec >> 16) & 0xFF);
        packet[offset + 2] = static_cast<u8>((ntp_sec >> 8)  & 0xFF);
        packet[offset + 3] = static_cast<u8>( ntp_sec        & 0xFF);

        // Write big-endian 32-bit fraction
        packet[offset + 4] = static_cast<u8>((ntp_frac >> 24) & 0xFF);
        packet[offset + 5] = static_cast<u8>((ntp_frac >> 16) & 0xFF);
        packet[offset + 6] = static_cast<u8>((ntp_frac >> 8)  & 0xFF);
        packet[offset + 7] = static_cast<u8>( ntp_frac        & 0xFF);
    }

    // =====================================================================
    // read_ntp_timestamp — read a 64-bit NTP timestamp from the buffer
    //
    // Returns microseconds since epoch.
    // =====================================================================
    static u64 read_ntp_timestamp(const u8 packet[NTP_PACKET_SIZE], u64 offset) noexcept {
        // Read big-endian 32-bit seconds
        u32 ntp_sec = (static_cast<u32>(packet[offset + 0]) << 24) |
                      (static_cast<u32>(packet[offset + 1]) << 16) |
                      (static_cast<u32>(packet[offset + 2]) << 8)  |
                       static_cast<u32>(packet[offset + 3]);

        // Read big-endian 32-bit fraction
        u32 ntp_frac = (static_cast<u32>(packet[offset + 4]) << 24) |
                       (static_cast<u32>(packet[offset + 5]) << 16) |
                       (static_cast<u32>(packet[offset + 6]) << 8)  |
                        static_cast<u32>(packet[offset + 7]);

        return ntp_to_us(ntp_sec, ntp_frac);
    }

    // =====================================================================
    // ntp_to_us — convert NTP (seconds since 1900, fraction 2^-32) to
    //             microseconds since epoch (1970-01-01)
    // =====================================================================
    static u64 ntp_to_us(u32 ntp_seconds, u32 ntp_fraction) noexcept {
        // Subtract epoch offset to get Unix seconds
        u64 unix_sec = static_cast<u64>(ntp_seconds);
        if (unix_sec < NTP_EPOCH_OFFSET_SEC) {
            // Time before 1970 — clamp to 0
            return 0;
        }
        unix_sec -= NTP_EPOCH_OFFSET_SEC;

        // Convert fraction (units of 2^-32) to microseconds
        // frac_us = ntp_fraction * 1e6 / 2^32
        // Use 64-bit arithmetic to avoid overflow
        u64 frac_us = (static_cast<u64>(ntp_fraction) * 1'000'000ULL) >> 32;

        return unix_sec * 1'000'000ULL + frac_us;
    }

    // =====================================================================
    // parse_ntp_response — extract timestamps and compute offset/RTT
    //
    // Packet layout:
    //   Offset 24-31: Originate Timestamp (T1 echoed back by server)
    //   Offset 32-39: Receive Timestamp   (T2 — when server received our request)
    //   Offset 40-47: Transmit Timestamp  (T3 — when server sent the response)
    //
    // Our send time T1 and receive time T4 come from the caller.
    //
    // Computation (RFC 5905 Section 8):
    //   offset = ((T2 - T1) + (T3 - T4)) / 2
    //   RTT    = (T4 - T1) - (T3 - T2)
    //
    // Returns None if the response is invalid (wrong mode, stratum=0, etc.)
    // =====================================================================
    static Opt<NTPResult> parse_ntp_response(const u8 packet[NTP_PACKET_SIZE],
                                               u64 t1_us, u64 t4_us) noexcept {
        // Validate mode: should be server (mode=4) in response
        u8 li_vn_mode = packet[NTP_LI_VN_MODE_OFFSET];
        u8 mode = li_vn_mode & 0x07;
        if (mode != NTP_MODE_SERVER) {
            return {}; // not a valid server response
        }

        // Validate stratum: 0 = kiss-o'-death / unspecified (reject)
        u8 stratum = packet[NTP_STRATUM_OFFSET];
        if (stratum == 0) {
            return {}; // server is not synchronized
        }

        // Read T2 (Receive Timestamp — when server got our request)
        u64 t2_us = read_ntp_timestamp(packet, NTP_RECV_TS_OFFSET);

        // Read T3 (Transmit Timestamp — when server sent the response)
        u64 t3_us = read_ntp_timestamp(packet, NTP_XMIT_TS_OFFSET);

        // Validate: timestamps must be non-zero
        if (t2_us == 0 || t3_us == 0) {
            return {};
        }

        // Compute offset and RTT using signed 64-bit arithmetic
        // T2 and T3 are in NTP epoch-shifted microseconds
        // T1 and T4 are in system-clock microseconds
        // offset = ((T2 - T1) + (T3 - T4)) / 2
        // RTT    = (T4 - T1) - (T3 - T2)

        i64 t1_i64 = static_cast<i64>(t1_us);
        i64 t2_i64 = static_cast<i64>(t2_us);
        i64 t3_i64 = static_cast<i64>(t3_us);
        i64 t4_i64 = static_cast<i64>(t4_us);

        i64 offset = ((t2_i64 - t1_i64) + (t3_i64 - t4_i64)) / 2;
        i64 rtt_i64 = (t4_i64 - t1_i64) - (t3_i64 - t2_i64);

        // Handle negative RTT (clock adjustment between T1 and T4)
        if (rtt_i64 < 0) rtt_i64 = 0;

        NTPResult result;
        result.offset_us     = offset;
        result.round_trip_ms = static_cast<f64>(rtt_i64) / 1000.0;
        result.server_time_us = t3_us;

        return Opt<NTPResult>{result};
    }

    // =====================================================================
    // udp_send_recv — send UDP packet to server:port and receive response
    //
    // Returns the number of bytes received, or None on error.
    // =====================================================================
    static Opt<u64> udp_send_recv(String_View server, u16 port,
                                    const u8* send_buf, u64 send_len,
                                    u8* recv_buf, u64 recv_len,
                                    u64 timeout_ms) noexcept {
#ifdef SPP_OS_LINUX
        // Create UDP socket
        i32 sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return {};

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec  = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Resolve hostname
        String<> host_s{server.length() + 1};
        host_s.set_length(server.length() + 1);
        for (u64 i = 0; i < server.length(); i++) host_s[i] = server[i];
        host_s[server.length()] = '\0';

        char port_buf[8];
        (void)Libc::snprintf(reinterpret_cast<u8*>(port_buf), 8, "%u", port);

        struct addrinfo hints = {};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo* result = null;
        i32 ret = ::getaddrinfo(reinterpret_cast<const char*>(host_s.data()),
                                 port_buf, &hints, &result);
        if (ret != 0 || !result) {
            ::close(sock);
            return {};
        }

        // Send UDP packet
        i64 sent = ::sendto(sock, send_buf, send_len, 0,
                             result->ai_addr,
                             static_cast<socklen_t>(result->ai_addrlen));
        if (sent != static_cast<i64>(send_len)) {
            ::freeaddrinfo(result);
            ::close(sock);
            return {};
        }

        // Receive response
        socklen_t addr_len = static_cast<socklen_t>(result->ai_addrlen);
        i64 received = ::recvfrom(sock, recv_buf, recv_len, 0,
                                   result->ai_addr, &addr_len);
        ::freeaddrinfo(result);
        ::close(sock);

        if (received < 0) {
            // Timeout or error
            return {};
        }

        return Opt<u64>{static_cast<u64>(received)};
#else
        (void)server; (void)port; (void)send_buf; (void)send_len;
        (void)recv_buf; (void)recv_len; (void)timeout_ms;
        return {};
#endif
    }
};

} // namespace spp::quant::monitoring

// =========================================================================
// SPP reflection records
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::monitoring::NTPClock, "NTPClock",
                 SPP_FIELD(offset_us_), SPP_FIELD(synchronized_));
