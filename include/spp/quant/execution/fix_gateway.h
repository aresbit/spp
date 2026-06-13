#pragma once

#ifndef SPP_BASE
#error "Include spp/core/base.h before quant headers."
#endif

#include "spp/quant/backtest/event.h"
#include "spp/quant/base/date.h"
#include "spp/quant/execution/order.h"
#include "spp/io/files.h"

#ifdef SPP_OS_LINUX
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace spp::quant::execution {

using spp::quant::backtest::OrderSide;
using spp::quant::backtest::OrderType;
using spp::quant::backtest::OrderStatus;
using spp::quant::backtest::FillEvent;

// =========================================================================
// FIX Protocol (Financial Information eXchange) 4.4
//
// SOH (Start of Header) = \x01 character, used as field delimiter.
// Wire format: tag=value<SOH>tag=value<SOH>...
// =========================================================================

constexpr String_View FIX_SOH = "\x01"_v;
constexpr char       FIX_SOH_CHAR = '\x01';

// Helpers: String_View utilities needed for FIX implementation
namespace fix_sv {

[[nodiscard]] inline bool starts_with(String_View s, String_View prefix) noexcept {
    if (s.length() < prefix.length()) return false;
    for (u64 i = 0; i < prefix.length(); i++) if (s[i] != prefix[i]) return false;
    return true;
}

[[nodiscard]] inline bool contains(String_View s, String_View sub) noexcept {
    if (sub.length() == 0) return true;
    if (sub.length() > s.length()) return false;
    for (u64 i = 0; i + sub.length() <= s.length(); i++) {
        bool match = true;
        for (u64 j = 0; j < sub.length(); j++) {
            if (s[i+j] != sub[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Write tag=value<SOH> into a String at byte position; return next write position
inline u64 write_field(String& s, u64 pos, u32 tag, String_View value) noexcept {
    // Write tag digits
    char tmp[12]; i32 p = 11; tmp[p] = '\0';
    u32 t = tag;
    if (t == 0) tmp[--p] = '0';
    else while (t > 0) { tmp[--p] = static_cast<char>('0' + (t % 10)); t /= 10; }
    for (i32 j = p; j < 11; j++) s[pos++] = static_cast<u8>(tmp[j]);
    s[pos++] = '=';
    for (u64 i = 0; i < value.length(); i++) s[pos++] = value[i];
    s[pos++] = static_cast<u8>(FIX_SOH_CHAR);
    return pos;
}

// Write raw bytes (tag part only, no '=') — for BeginString, BodyLength, CheckSum
inline u64 write_tag_prefix(String& s, u64 pos, u32 tag) noexcept {
    char tmp[12]; i32 p = 11; tmp[p] = '\0';
    u32 t = tag;
    if (t == 0) tmp[--p] = '0';
    else while (t > 0) { tmp[--p] = static_cast<char>('0' + (t % 10)); t /= 10; }
    for (i32 j = p; j < 11; j++) s[pos++] = static_cast<u8>(tmp[j]);
    return pos;
}

} // namespace fix_sv

// =========================================================================
// FIXTag — standard field tag constants (FIX 4.4)
// =========================================================================
namespace FIXTag {
    constexpr u32 BeginString     = 8;
    constexpr u32 BodyLength      = 9;
    constexpr u32 MsgType         = 35;
    constexpr u32 MsgSeqNum       = 34;
    constexpr u32 SenderCompID    = 49;
    constexpr u32 TargetCompID    = 56;
    constexpr u32 SendingTime     = 52;
    constexpr u32 OrigSendingTime = 122;
    constexpr u32 OrdStatus       = 39;
    constexpr u32 ClOrdID         = 11;
    constexpr u32 OrigClOrdID     = 41;
    constexpr u32 OrderID         = 37;
    constexpr u32 ExecID          = 17;
    constexpr u32 Symbol          = 55;
    constexpr u32 Side            = 54;
    constexpr u32 OrderQty        = 38;
    constexpr u32 OrdType         = 40;
    constexpr u32 Price           = 44;
    constexpr u32 StopPx          = 99;
    constexpr u32 TimeInForce     = 59;
    constexpr u32 LastQty         = 32;
    constexpr u32 LastPx          = 31;
    constexpr u32 LeavesQty       = 151;
    constexpr u32 CumQty          = 14;
    constexpr u32 AvgPx           = 6;
    constexpr u32 TransactTime    = 60;
    constexpr u32 ExecType        = 150;
    constexpr u32 ExecTransType   = 20;
    constexpr u32 OrdRejReason    = 103;
    constexpr u32 Text            = 58;
    constexpr u32 CheckSum        = 10;
    constexpr u32 Account         = 1;
    constexpr u32 SecurityID      = 48;
    constexpr u32 SecurityIDSource = 22;
    constexpr u32 HandlInst       = 21;
    constexpr u32 MaxFloor        = 111;
    constexpr u32 MinQty          = 110;
    constexpr u32 Password        = 554;
    constexpr u32 Username        = 553;
    constexpr u32 HeartBtInt      = 108;
    constexpr u32 EncryptMethod   = 98;
    constexpr u32 ResetSeqNumFlag = 141;
    constexpr u32 TestReqID       = 112;
    constexpr u32 BeginSeqNo      = 7;
    constexpr u32 EndSeqNo        = 16;
    constexpr u32 NewSeqNo        = 36;
    constexpr u32 GapFillFlag     = 123;
    constexpr u32 RefMsgType      = 372;
    constexpr u32 RefSeqNum       = 45;
    constexpr u32 PossDupFlag     = 43;
}

// =========================================================================
// FIXMsgType — message type string constants
// =========================================================================
namespace FIXMsgType {
    constexpr String_View Heartbeat            = "0"_v;
    constexpr String_View TestRequest          = "1"_v;
    constexpr String_View ResendRequest        = "2"_v;
    constexpr String_View Reject               = "3"_v;
    constexpr String_View SequenceReset        = "4"_v;
    constexpr String_View Logout               = "5"_v;
    constexpr String_View ExecutionReport      = "8"_v;
    constexpr String_View OrderCancelReject    = "9"_v;
    constexpr String_View Logon                = "A"_v;
    constexpr String_View NewOrderSingle       = "D"_v;
    constexpr String_View OrderCancelRequest   = "F"_v;
    constexpr String_View OrderCancelReplace   = "G"_v;
    constexpr String_View OrderStatusRequest   = "H"_v;
}

namespace FIXSide    { constexpr String_View Buy="1"_v, Sell="2"_v, SellShort="5"_v; }
namespace FIXOrdType { constexpr String_View Market="1"_v, Limit="2"_v, Stop="3"_v, StopLimit="4"_v; }
namespace FIXExecType {
    constexpr String_View New="0"_v, Partial="1"_v, Fill="2"_v, Done="3"_v,
                         Cancelled="4"_v, Replaced="5"_v, Rejected="8"_v;
}
namespace FIXOrdStatus {
    constexpr String_View New="0"_v, Partial="1"_v, Filled="2"_v, Done="3"_v,
                         Cancelled="4"_v, Replaced="5"_v, PendingCancel="6"_v, Rejected="8"_v;
}

// =========================================================================
// FIXField — a single tag=value pair
// =========================================================================
struct FIXField {
    u32 tag_ = 0;
    String value_;

    static FIXField make(u32 tag, String value) noexcept {
        return FIXField{tag, spp::move(value)};
    }

    static FIXField make_int(u32 tag, i64 value) noexcept {
        String s;
        if (value == 0) { s = String{1}; s.set_length(1); s[0] = '0'; return FIXField{tag, spp::move(s)}; }
        bool neg = value < 0;
        if (neg) value = -value;
        char buf[24]; i32 p = 23; buf[p] = '\0';
        while (value > 0) { buf[--p] = static_cast<char>('0' + (value % 10)); value /= 10; }
        if (neg) buf[--p] = '-';
        i32 len = 23 - p;
        s = String{static_cast<u64>(len)}; s.set_length(static_cast<u64>(len));
        for (i32 i = p; i < 23; i++) s[static_cast<u64>(i - p)] = static_cast<u8>(buf[i]);
        return FIXField{tag, spp::move(s)};
    }

    static FIXField make_float(u32 tag, f64 value, u8 precision = 2) noexcept {
        // Build integer and fractional parts
        Vec<char> buf;
        buf.reserve(32);
        if (value < 0.0) { buf.push('-'); value = -value; }
        i64 int_part = static_cast<i64>(value);
        f64 frac = value - static_cast<f64>(int_part);

        char ibuf[24]; i32 ip = 23; ibuf[ip] = '\0';
        if (int_part == 0) ibuf[--ip] = '0';
        else { i64 ipv = int_part; while (ipv > 0) { ibuf[--ip] = static_cast<char>('0' + (ipv % 10)); ipv /= 10; } }
        for (i32 i = ip; i < 23; i++) buf.push(ibuf[i]);

        if (precision > 0) {
            buf.push('.');
            for (u8 p = 0; p < precision; p++) {
                frac *= 10.0;
                i32 digit = static_cast<i32>(frac);
                buf.push(static_cast<char>('0' + digit));
                frac -= static_cast<f64>(digit);
            }
        }

        String s{static_cast<u64>(buf.length())};
        s.set_length(static_cast<u64>(buf.length()));
        for (u64 i = 0; i < buf.length(); i++) s[i] = static_cast<u8>(buf[i]);
        return FIXField{tag, spp::move(s)};
    }

    static FIXField make_date(u32 tag, Date date) noexcept {
        auto ymd = date.ymd();
        i32 y = ymd.get<0>(); u32 m = ymd.get<1>(), d = ymd.get<2>();
        String s{8}; s.set_length(8);
        s[0] = static_cast<u8>('0' + (y/1000)); s[1] = static_cast<u8>('0' + ((y/100)%10));
        s[2] = static_cast<u8>('0' + ((y/10)%10)); s[3] = static_cast<u8>('0' + (y%10));
        s[4] = static_cast<u8>('0' + (m/10)); s[5] = static_cast<u8>('0' + (m%10));
        s[6] = static_cast<u8>('0' + (d/10)); s[7] = static_cast<u8>('0' + (d%10));
        return FIXField{tag, spp::move(s)};
    }

    static FIXField make_time(u32 tag, Date datetime) noexcept {
        auto ymd = datetime.ymd();
        i32 y = ymd.get<0>(); u32 m = ymd.get<1>(), d = ymd.get<2>();
        // YYYYMMDD-HH:MM:SS (fixed length 17)
        String s{17}; s.set_length(17);
        s[0]=static_cast<u8>('0'+(y/1000)); s[1]=static_cast<u8>('0'+((y/100)%10));
        s[2]=static_cast<u8>('0'+((y/10)%10)); s[3]=static_cast<u8>('0'+(y%10));
        s[4]=static_cast<u8>('0'+(m/10)); s[5]=static_cast<u8>('0'+(m%10));
        s[6]=static_cast<u8>('0'+(d/10)); s[7]=static_cast<u8>('0'+(d%10));
        s[8]='-'; s[9]='0'; s[10]='0'; s[11]=':'; s[12]='0'; s[13]='0'; s[14]=':'; s[15]='0'; s[16]='0';
        return FIXField{tag, spp::move(s)};
    }
};

// =========================================================================
// FIXMessage — a complete FIX message
// =========================================================================
struct FIXMessage {
    String begin_string_ = "FIX.4.4"_v;
    String msg_type_;
    Vec<FIXField> fields_;

    FIXMessage() = default;
    explicit FIXMessage(String_View type) : msg_type_(type.length() > 0 ? String{type} : String{}) {}

    void add_field(FIXField field) noexcept { fields_.push(spp::move(field)); }
    void add_field(u32 tag, String value) noexcept { fields_.push(FIXField::make(tag, spp::move(value))); }

    [[nodiscard]] Opt<String> get_field(u32 tag) const noexcept {
        for (u64 i = 0; i < fields_.length(); i++)
            if (fields_[i].tag_ == tag) return Opt<String>{String{fields_[i].value_}};
        return {};
    }

    [[nodiscard]] Opt<f64> get_float(u32 tag) const noexcept {
        auto v = get_field(tag);
        if (!v.ok()) return {};
        String_View s = v->view();
        f64 result = 0.0, sign = 1.0;
        u64 pos = 0;
        if (!s.empty() && static_cast<char>(s[0]) == '-') { sign = -1.0; pos++; }
        f64 frac = 0.1;
        bool in_frac = false;
        while (pos < s.length()) {
            char c = static_cast<char>(s[pos]);
            if (c >= '0' && c <= '9') {
                if (in_frac) { result += static_cast<f64>(c - '0') * frac; frac *= 0.1; }
                else result = result * 10.0 + static_cast<f64>(c - '0');
            } else if (c == '.') in_frac = true;
            pos++;
        }
        return Opt<f64>{result * sign};
    }

    [[nodiscard]] Opt<i64> get_int(u32 tag) const noexcept {
        auto v = get_field(tag);
        if (!v.ok()) return {};
        String_View s = v->view();
        i64 result = 0, sign = 1;
        u64 pos = 0;
        if (!s.empty() && static_cast<char>(s[0]) == '-') { sign = -1; pos++; }
        while (pos < s.length() && s[pos] >= '0' && s[pos] <= '9') {
            result = result * 10 + static_cast<i64>(s[pos] - '0');
            pos++;
        }
        return Opt<i64>{result * sign};
    }

    [[nodiscard]] String_View msg_type() const noexcept { return msg_type_.view(); }

    [[nodiscard]] bool is_admin() const noexcept {
        if (msg_type_.empty()) return false;
        char c = static_cast<char>(msg_type_[0]);
        return (c >= '0' && c <= '5') || c == 'A';
    }

    [[nodiscard]] bool is_app() const noexcept { return !is_admin(); }

    // ==================================================================
    // Body length: total bytes of all fields except CheckSum(10)
    // ==================================================================
    [[nodiscard]] u64 body_length() const noexcept {
        u64 total = 0;
        // MsgType: 35= + value + SOH
        total += 3 + msg_type_.length() + 1;

        for (u64 i = 0; i < fields_.length(); i++) {
            const FIXField& f = fields_[i];
            if (f.tag_ == FIXTag::CheckSum) continue;
            // tag digits + '=' + value + SOH
            total += f.value_.length() + 2; // '=' + SOH
            u32 t = f.tag_;
            if (t < 10) total += 1;
            else if (t < 100) total += 2;
            else if (t < 1000) total += 3;
            else if (t < 10000) total += 4;
            else total += 5;
        }
        return total;
    }

    // ==================================================================
    // Checksum: sum of all bytes % 256, 3-digit zero-padded
    // ==================================================================
    [[nodiscard]] static String checksum_for(String_View raw) noexcept {
        u64 sum = 0;
        for (u64 i = 0; i < raw.length(); i++) sum += raw[i];
        u64 chk = sum % 256;
        String s{3}; s.set_length(3);
        s[0] = static_cast<u8>('0' + (chk / 100));
        s[1] = static_cast<u8>('0' + ((chk / 10) % 10));
        s[2] = static_cast<u8>('0' + (chk % 10));
        return s;
    }

    // ==================================================================
    // Encode to wire format
    // ==================================================================
    [[nodiscard]] String encode() const noexcept {
        // First pass: compute body length
        u64 body_len = body_length();

        // Format body length as string
        char bl_buf[8]; i32 blp = 7; bl_buf[blp] = '\0';
        u64 bl = body_len;
        if (bl == 0) bl_buf[--blp] = '0';
        else { while (bl > 0) { bl_buf[--blp] = static_cast<char>('0' + (bl % 10)); bl /= 10; } }
        u64 bl_len = static_cast<u64>(7 - blp);

        // Compute total output size
        u64 total = 0;
        total += 2 + begin_string_.length() + 1 + 1; // 8=FIX.4.4<SOH>
        total += 2 + bl_len + 1 + 1;                  // 9=NNN<SOH>
        total += 3 + msg_type_.length() + 1;          // 35=D<SOH>
        for (u64 i = 0; i < fields_.length(); i++) {
            const FIXField& f = fields_[i];
            if (f.tag_ == FIXTag::CheckSum) continue;
            total += f.value_.length() + 2;
            u32 t = f.tag_;
            if (t < 10) total += 1;
            else if (t < 100) total += 2;
            else if (t < 1000) total += 3;
            else if (t < 10000) total += 4;
            else total += 5;
        }
        // Build everything EXCEPT checksum to compute it
        u64 raw_len = total;
        String raw{raw_len}; raw.set_length(raw_len);
        u64 pos = 0;
        pos = fix_sv::write_tag_prefix(raw, pos, FIXTag::BeginString);
        raw[pos++] = '=';
        pos = fix_sv::write_field(raw, pos - 1, 0, begin_string_.view());
        // The above is wrong — write_field writes tag=value<SOH>, but we already wrote tag.
        // Let me rewrite this more carefully.
        // Actually, let me just rebuild raw properly:

        // Rewind and rebuild
        pos = 0;
        // 8=FIX.4.4<SOH>
        pos = fix_sv::write_field(raw, pos, FIXTag::BeginString, begin_string_.view());
        // 9=NNN<SOH>
        String_View bl_sv{reinterpret_cast<const u8*>(bl_buf + blp), bl_len};
        pos = fix_sv::write_field(raw, pos, FIXTag::BodyLength, bl_sv);
        // 35=D<SOH>
        pos = fix_sv::write_field(raw, pos, FIXTag::MsgType, msg_type_.view());
        // All fields except 10 (CheckSum)
        for (u64 i = 0; i < fields_.length(); i++) {
            const FIXField& f = fields_[i];
            if (f.tag_ == FIXTag::CheckSum) continue;
            pos = fix_sv::write_field(raw, pos, f.tag_, f.value_.view());
        }

        // Compute checksum
        String cs = checksum_for(raw.view());

        // Now encode final with checksum
        u64 final_len = raw_len + 3 + cs.length() + 1; // 10=XXX<SOH>
        String final_str{final_len}; final_str.set_length(final_len);
        u64 fp = 0;
        for (u64 i = 0; i < raw_len; i++) final_str[fp++] = raw[i];
        fp = fix_sv::write_field(final_str, fp, FIXTag::CheckSum, cs.view());

        return final_str;
    }

    [[nodiscard]] String checksum() const noexcept {
        // Build everything except checksum, compute it
        // (Reuse the body of encode())
        u64 body_len = body_length();
        char bl_buf[8]; i32 blp = 7; bl_buf[blp] = '\0';
        u64 bl = body_len;
        if (bl == 0) bl_buf[--blp] = '0';
        else { while (bl > 0) { bl_buf[--blp] = static_cast<char>('0' + (bl % 10)); bl /= 10; } }
        u64 bl_len = static_cast<u64>(7 - blp);

        u64 raw_len = 0;
        raw_len += 2 + begin_string_.length() + 1;
        raw_len += 2 + bl_len + 1;
        raw_len += 3 + msg_type_.length() + 1;
        for (u64 i = 0; i < fields_.length(); i++) {
            const FIXField& f = fields_[i];
            if (f.tag_ == FIXTag::CheckSum) continue;
            raw_len += f.value_.length() + 2;
            u32 t = f.tag_;
            if (t < 10) raw_len += 1;
            else if (t < 100) raw_len += 2;
            else if (t < 1000) raw_len += 3;
            else if (t < 10000) raw_len += 4;
            else raw_len += 5;
        }

        String raw{raw_len}; raw.set_length(raw_len);
        u64 pos = 0;
        pos = fix_sv::write_field(raw, pos, FIXTag::BeginString, begin_string_.view());
        String_View bl_sv{reinterpret_cast<const u8*>(bl_buf + blp), bl_len};
        pos = fix_sv::write_field(raw, pos, FIXTag::BodyLength, bl_sv);
        pos = fix_sv::write_field(raw, pos, FIXTag::MsgType, msg_type_.view());
        for (u64 i = 0; i < fields_.length(); i++) {
            const FIXField& f = fields_[i];
            if (f.tag_ == FIXTag::CheckSum) continue;
            pos = fix_sv::write_field(raw, pos, f.tag_, f.value_.view());
        }
        return checksum_for(raw.view());
    }

    // ==================================================================
    // Parse from wire format
    // ==================================================================
    static Opt<FIXMessage> parse(String_View raw) noexcept {
        if (raw.empty()) return {};
        FIXMessage msg;
        u64 pos = 0;
        bool parsed_type = false;

        while (pos < raw.length()) {
            u64 end = pos;
            while (end < raw.length() && raw[end] != static_cast<u8>(FIX_SOH_CHAR)) end++;
            if (end == pos) { pos = end + 1; continue; }

            String_View kv = raw.sub(pos, end);
            u64 eq_pos = 0;
            for (; eq_pos < kv.length(); eq_pos++)
                if (static_cast<char>(kv[eq_pos]) == '=') break;
            if (eq_pos >= kv.length()) { pos = end + 1; continue; }

            String_View tag_str = kv.sub(0, eq_pos);
            String_View val_str = kv.sub(eq_pos + 1, kv.length());

            u32 tag = 0;
            for (u64 i = 0; i < tag_str.length() && tag_str[i] >= '0' && tag_str[i] <= '9'; i++)
                tag = tag * 10 + static_cast<u32>(tag_str[i] - '0');

            if (tag == FIXTag::CheckSum) { pos = end + 1; continue; }
            if (tag == FIXTag::BeginString) msg.begin_string_ = String{val_str};
            if (tag == FIXTag::MsgType) { msg.msg_type_ = String{val_str}; parsed_type = true; }
            msg.fields_.push(FIXField::make(tag, String{val_str}));

            pos = end + 1;
        }

        if (!parsed_type) return {};
        return Opt<FIXMessage>{spp::move(msg)};
    }
};

// =========================================================================
// FIX session state
// =========================================================================
enum struct FIXSessionState : u8 {
    Disconnected, Connecting, Connected, LoggedIn, LoggingOut, Reconnecting
};

// =========================================================================
// FIXSession — FIX 4.4 protocol engine (client-side)
// =========================================================================
struct FIXSession {
    String sender_comp_id_;
    String target_comp_id_;
    String username_;
    String password_;
    u32 heartbeat_interval_ = 30;

    u64 socket_fd_ = ~u64{0};
    FIXSessionState state_ = FIXSessionState::Disconnected;

    u64 seq_num_out_ = 1;
    u64 seq_num_in_  = 1;

    Vec<FIXMessage> sent_messages_;

    struct PendingOrder {
        String cl_ord_id_;
        u64 timestamp_;
    };
    Map<String, PendingOrder> pending_orders_;

    Vec<u8> recv_buf_;

    FIXSession() = default;
    explicit FIXSession(String_View sender, String_View target) noexcept
        : sender_comp_id_(String{sender}), target_comp_id_(String{target}) {
        recv_buf_.reserve(8192);
        sent_messages_.reserve(256);
    }
    ~FIXSession() noexcept { disconnect(); }

    // ==================================================================
    // TCP Socket operations
    // ==================================================================
#ifdef SPP_OS_LINUX
    static bool sock_send(u64 fd, String_View data) noexcept {
        auto n = ::send(static_cast<i32>(fd), data.data(), data.length(), MSG_NOSIGNAL);
        return n == static_cast<i64>(data.length());
    }

    static Opt<String> sock_recv(u64 fd, u64 timeout_ms) noexcept {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(static_cast<i32>(fd), &rfds);
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
        i32 ret = ::select(static_cast<i32>(fd) + 1, &rfds, null, null, &tv);
        if (ret <= 0) return {};
        u8 buf[8192];
        auto n = ::recv(static_cast<i32>(fd), buf, sizeof(buf), 0);
        if (n <= 0) return {};
        String result{static_cast<u64>(n)};
        result.set_length(static_cast<u64>(n));
        for (i64 i = 0; i < n; i++) result[static_cast<u64>(i)] = buf[i];
        return Opt<String>{spp::move(result)};
    }

    static void sock_close(u64 fd) noexcept {
        if (fd != ~u64{0}) ::close(static_cast<i32>(fd));
    }
#else
    static bool sock_send(u64, String_View) noexcept { return false; }
    static Opt<String> sock_recv(u64, u64) noexcept { return {}; }
    static void sock_close(u64) noexcept {}
#endif

    // ==================================================================
    // Connection management
    // ==================================================================
    bool connect(String_View host, u16 port) noexcept {
#ifdef SPP_OS_LINUX
        i32 fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        i32 flag = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        // Build host string
        String host_s{host.length() + 1};
        host_s.set_length(host.length() + 1);
        for (u64 i = 0; i < host.length(); i++) host_s[i] = host[i];
        host_s[host.length()] = '\0';

        char port_s[8];
        Libc::snprintf(reinterpret_cast<u8*>(port_s), 8, "%u", port);

        struct addrinfo* result = null;
        i32 ret = ::getaddrinfo(reinterpret_cast<const char*>(host_s.data()), port_s, &hints, &result);
        if (ret != 0 || !result) return false;

        i32 flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        i32 conn_ret = ::connect(fd, result->ai_addr, static_cast<socklen_t>(result->ai_addrlen));
        ::freeaddrinfo(result);

        if (conn_ret < 0 && errno == EINPROGRESS) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
            struct timeval tv; tv.tv_sec = 10; tv.tv_usec = 0;
            if (::select(fd + 1, null, &wfds, null, &tv) <= 0) { ::close(fd); return false; }
        }
        ::fcntl(fd, F_SETFL, flags);
        socket_fd_ = static_cast<u64>(fd);
        state_ = FIXSessionState::Connected;
        return true;
#else
        (void)host; (void)port;
        return false;
#endif
    }

    bool logon(u32 heartbeat_interval = 30) noexcept {
        if (state_ != FIXSessionState::Connected) return false;
        heartbeat_interval_ = heartbeat_interval;
        FIXMessage logon_msg = build_logon(heartbeat_interval);
        if (!send(logon_msg)) return false;
        auto resp = recv(5000);
        if (!resp.ok()) return false;
        if (resp->msg_type() == FIXMsgType::Logon) { handle_logon(*resp); return state_ == FIXSessionState::LoggedIn; }
        if (resp->msg_type() == FIXMsgType::Reject) { handle_reject(*resp); return false; }
        return false;
    }

    bool logout(String_View reason = ""_v) noexcept {
        if (state_ != FIXSessionState::LoggedIn && state_ != FIXSessionState::Connected) return false;
        state_ = FIXSessionState::LoggingOut;
        FIXMessage msg = build_logout(reason);
        send(msg);
        auto resp = recv(5000);
        if (resp.ok() && resp->msg_type() == FIXMsgType::Logout) state_ = FIXSessionState::Disconnected;
        disconnect();
        return true;
    }

    void disconnect() noexcept {
        sock_close(socket_fd_); socket_fd_ = ~u64{0};
        state_ = FIXSessionState::Disconnected;
        seq_num_out_ = 1; seq_num_in_ = 1;
        sent_messages_.clear(); pending_orders_.clear(); recv_buf_.clear();
    }

    void heartbeat() noexcept {
        if (state_ != FIXSessionState::LoggedIn) return;
        FIXMessage hb; hb.msg_type_ = String{FIXMsgType::Heartbeat}; send(hb);
    }

    void poll() noexcept {
        if (state_ != FIXSessionState::LoggedIn && state_ != FIXSessionState::Connected) return;
        auto msg = recv(0);
        if (msg.ok()) handle_message(*msg);
    }

    // ==================================================================
    // Send a FIX message
    // ==================================================================
    bool send(FIXMessage& msg) noexcept {
        if (socket_fd_ == ~u64{0}) return false;

        // Auto-assign fields
        bool has_seq = false, has_sender = false, has_target = false, has_time = false;
        for (u64 i = 0; i < msg.fields_.length(); i++) {
            u32 t = msg.fields_[i].tag_;
            if (t == FIXTag::MsgSeqNum) has_seq = true;
            if (t == FIXTag::SenderCompID) has_sender = true;
            if (t == FIXTag::TargetCompID) has_target = true;
            if (t == FIXTag::SendingTime) has_time = true;
        }
        if (!has_seq) { msg.fields_.push(FIXField::make_int(FIXTag::MsgSeqNum, static_cast<i64>(seq_num_out_))); seq_num_out_++; }
        if (!has_sender) msg.fields_.push(FIXField::make(FIXTag::SenderCompID, String{sender_comp_id_}));
        if (!has_target) msg.fields_.push(FIXField::make(FIXTag::TargetCompID, String{target_comp_id_}));
        if (!has_time) msg.fields_.push(FIXField::make_time(FIXTag::SendingTime, Date::today()));

        String wire = msg.encode();
        if (!sock_send(socket_fd_, wire.view())) return false;
        store_sent_message(msg);
        return true;
    }

    Opt<FIXMessage> recv(u64 timeout_ms = 1000) noexcept {
        if (socket_fd_ == ~u64{0}) return {};
        auto result = try_parse_buffer();
        if (result.ok()) return result;
        auto data = sock_recv(socket_fd_, timeout_ms);
        if (!data.ok()) return {};
        String_View sv = data->view();
        for (u64 i = 0; i < sv.length(); i++) recv_buf_.push(sv[i]);
        return try_parse_buffer();
    }

    // ==================================================================
    // Message handling
    // ==================================================================
    void handle_message(const FIXMessage& msg) noexcept {
        String_View mt = msg.msg_type();
        if (mt == FIXMsgType::Logon) handle_logon(msg);
        else if (mt == FIXMsgType::Logout) handle_logout(msg);
        else if (mt == FIXMsgType::Heartbeat) handle_heartbeat(msg);
        else if (mt == FIXMsgType::TestRequest) handle_test_request(msg);
        else if (mt == FIXMsgType::ResendRequest) handle_resend_request(msg);
        else if (mt == FIXMsgType::SequenceReset) handle_sequence_reset(msg);
        else if (mt == FIXMsgType::Reject) handle_reject(msg);
    }

    // ==================================================================
    // Application messages
    // ==================================================================

    String send_new_order(String_View symbol, OrderSide side, OrderType type,
                          f64 quantity, f64 price = 0.0, f64 stop_price = 0.0,
                          String_View time_in_force = "DAY"_v) noexcept {
        if (state_ != FIXSessionState::LoggedIn) return String{};

        static u64 clord_ctr = 0; clord_ctr++;
        Vec<char> cl_buf; cl_buf.reserve(32);
        for (char c : "CLORD_"_v) cl_buf.push(c);
        char cbuf[24]; u64 cc = clord_ctr; i32 cp = 23; cbuf[cp] = '\0';
        if (cc == 0) cbuf[--cp] = '0';
        else while (cc > 0) { cbuf[--cp] = static_cast<char>('0' + (cc % 10)); cc /= 10; }
        for (i32 i = cp; i < 23; i++) cl_buf.push(cbuf[i]);
        String cl_ord_id{static_cast<u64>(cl_buf.length())};
        cl_ord_id.set_length(static_cast<u64>(cl_buf.length()));
        for (u64 i = 0; i < cl_buf.length(); i++) cl_ord_id[i] = static_cast<u8>(cl_buf[i]);

        FIXMessage msg{FIXMsgType::NewOrderSingle};
        msg.fields_.push(FIXField::make(FIXTag::ClOrdID, String{cl_ord_id}));
        msg.fields_.push(FIXField::make(FIXTag::Symbol, String{symbol}));

        String_View side_str = (side == OrderSide::Buy) ? FIXSide::Buy : FIXSide::Sell;
        msg.fields_.push(FIXField::make(FIXTag::Side, String{side_str}));
        msg.fields_.push(FIXField::make_float(FIXTag::OrderQty, quantity, 0));

        String_View type_str;
        switch (type) {
        case OrderType::Market: type_str = FIXOrdType::Market; break;
        case OrderType::Limit:  type_str = FIXOrdType::Limit; break;
        case OrderType::Stop:   type_str = FIXOrdType::Stop; break;
        case OrderType::StopLimit: type_str = FIXOrdType::StopLimit; break;
        default: type_str = FIXOrdType::Market; break;
        }
        msg.fields_.push(FIXField::make(FIXTag::OrdType, String{type_str}));

        if (price > 0.0 && type != OrderType::Market)
            msg.fields_.push(FIXField::make_float(FIXTag::Price, price, 2));
        if (stop_price > 0.0 && (type == OrderType::Stop || type == OrderType::StopLimit))
            msg.fields_.push(FIXField::make_float(FIXTag::StopPx, stop_price, 2));
        msg.fields_.push(FIXField::make(FIXTag::TimeInForce, String{time_in_force}));
        msg.fields_.push(FIXField::make_int(FIXTag::HandlInst, 1));

        if (send(msg)) {
            pending_orders_.insert(String{cl_ord_id},
                                    PendingOrder{String{cl_ord_id}, 0});
            return cl_ord_id;
        }
        return String{};
    }

    bool send_cancel_order(String_View orig_cl_ord_id, String_View symbol, OrderSide side) noexcept {
        if (state_ != FIXSessionState::LoggedIn) return false;
        static u64 cancel_ctr = 0; cancel_ctr++;
        Vec<char> cl_buf; cl_buf.reserve(32);
        for (char c : "CANCEL_"_v) cl_buf.push(c);
        char cbuf[24]; u64 cc = cancel_ctr; i32 cp = 23; cbuf[cp] = '\0';
        if (cc == 0) cbuf[--cp] = '0';
        else while (cc > 0) { cbuf[--cp] = static_cast<char>('0' + (cc % 10)); cc /= 10; }
        for (i32 i = cp; i < 23; i++) cl_buf.push(cbuf[i]);
        String cl_ord_id{static_cast<u64>(cl_buf.length())};
        cl_ord_id.set_length(static_cast<u64>(cl_buf.length()));
        for (u64 i = 0; i < cl_buf.length(); i++) cl_ord_id[i] = static_cast<u8>(cl_buf[i]);

        FIXMessage msg{FIXMsgType::OrderCancelRequest};
        msg.fields_.push(FIXField::make(FIXTag::ClOrdID, String{cl_ord_id}));
        msg.fields_.push(FIXField::make(FIXTag::OrigClOrdID, String{orig_cl_ord_id}));
        msg.fields_.push(FIXField::make(FIXTag::Symbol, String{symbol}));
        String_View side_str = (side == OrderSide::Buy) ? FIXSide::Buy : FIXSide::Sell;
        msg.fields_.push(FIXField::make(FIXTag::Side, String{side_str}));
        return send(msg);
    }

    bool send_replace_order(String_View orig_cl_ord_id, String_View cl_ord_id,
                            String_View symbol, OrderSide side, f64 new_qty, f64 new_price) noexcept {
        if (state_ != FIXSessionState::LoggedIn) return false;
        FIXMessage msg{FIXMsgType::OrderCancelReplace};
        msg.fields_.push(FIXField::make(FIXTag::ClOrdID, String{cl_ord_id}));
        msg.fields_.push(FIXField::make(FIXTag::OrigClOrdID, String{orig_cl_ord_id}));
        msg.fields_.push(FIXField::make(FIXTag::Symbol, String{symbol}));
        msg.fields_.push(FIXField::make(FIXTag::Side, String{(side == OrderSide::Buy) ? FIXSide::Buy : FIXSide::Sell}));
        msg.fields_.push(FIXField::make_float(FIXTag::OrderQty, new_qty, 0));
        msg.fields_.push(FIXField::make_float(FIXTag::Price, new_price, 2));
        return send(msg);
    }

    bool send_order_status_request(String_View cl_ord_id, String_View symbol, OrderSide side) noexcept {
        if (state_ != FIXSessionState::LoggedIn) return false;
        FIXMessage msg{FIXMsgType::OrderStatusRequest};
        msg.fields_.push(FIXField::make(FIXTag::ClOrdID, String{cl_ord_id}));
        msg.fields_.push(FIXField::make(FIXTag::Symbol, String{symbol}));
        msg.fields_.push(FIXField::make(FIXTag::Side, String{(side == OrderSide::Buy) ? FIXSide::Buy : FIXSide::Sell}));
        return send(msg);
    }

    // Parse an ExecutionReport into a FillEvent
    [[nodiscard]] Opt<FillEvent> parse_execution_report(const FIXMessage& msg) const noexcept {
        if (msg.msg_type() != FIXMsgType::ExecutionReport) return {};

        auto exec_type = msg.get_field(FIXTag::ExecType);
        if (exec_type.ok()) {
            String_View et = exec_type->view();
            if (et != FIXExecType::Fill && et != FIXExecType::Partial) return {};
        }

        FillEvent fill;
        fill.fill_id_ = 0; fill.order_id_ = 0; fill.date_ = Date::today();
        fill.side_ = OrderSide::Buy; fill.quantity_ = 0.0; fill.price_ = 0.0;
        fill.commission_ = 0.0; fill.slippage_ = 0.0;

        auto exec_id = msg.get_int(FIXTag::ExecID);
        if (exec_id.ok()) fill.fill_id_ = static_cast<u64>(*exec_id);

        auto order_id = msg.get_field(FIXTag::OrderID);
        if (order_id.ok()) fill.order_id_ = order_id->view().hash();

        auto side_str = msg.get_field(FIXTag::Side);
        if (side_str.ok()) fill.side_ = (side_str->view() == FIXSide::Buy) ? OrderSide::Buy : OrderSide::Sell;

        auto last_qty = msg.get_float(FIXTag::LastQty);
        if (last_qty.ok()) fill.quantity_ = *last_qty;

        auto last_px = msg.get_float(FIXTag::LastPx);
        if (last_px.ok()) fill.price_ = *last_px;

        return Opt<FillEvent>{spp::move(fill)};
    }

private:
    [[nodiscard]] Opt<FIXMessage> try_parse_buffer() noexcept {
        if (recv_buf_.length() < 20) return {};
        for (u64 i = 0; i + 6 < recv_buf_.length(); i++) {
            if (recv_buf_[i] == static_cast<u8>(FIX_SOH_CHAR) &&
                recv_buf_[i+1] == '1' && recv_buf_[i+2] == '0' && recv_buf_[i+3] == '=') {
                u64 cs_end = i + 4;
                while (cs_end < recv_buf_.length() && recv_buf_[cs_end] != static_cast<u8>(FIX_SOH_CHAR)) cs_end++;
                if (cs_end < recv_buf_.length() && recv_buf_[cs_end] == static_cast<u8>(FIX_SOH_CHAR)) {
                    String_View raw{reinterpret_cast<const char*>(recv_buf_.data()), cs_end + 1};
                    auto msg = FIXMessage::parse(raw);
                    u64 consumed = cs_end + 1;
                    for (u64 j = 0; j < recv_buf_.length() - consumed; j++)
                        recv_buf_[j] = recv_buf_[j + consumed];
                    for (u64 j = 0; j < consumed; j++) recv_buf_.pop();
                    return msg;
                }
            }
        }
        return {};
    }

    void handle_logon(const FIXMessage& msg) noexcept {
        if (!validate_seq_num(msg)) return;
        auto rf = msg.get_int(FIXTag::ResetSeqNumFlag);
        if (rf.ok() && *rf == 1) { seq_num_out_ = 1; seq_num_in_ = 1; }
        state_ = FIXSessionState::LoggedIn;
    }

    void handle_logout(const FIXMessage& msg) noexcept {
        validate_seq_num(msg);
        state_ = FIXSessionState::Disconnected;
    }

    void handle_heartbeat(const FIXMessage& msg) noexcept { validate_seq_num(msg); }

    void handle_test_request(const FIXMessage& msg) noexcept {
        validate_seq_num(msg);
        auto tri = msg.get_field(FIXTag::TestReqID);
        FIXMessage hb;
        hb.msg_type_ = String{FIXMsgType::Heartbeat};
        if (tri.ok()) hb.fields_.push(FIXField::make(FIXTag::TestReqID, String{tri->view()}));
        send(hb);
    }

    void handle_resend_request(const FIXMessage& msg) noexcept {
        validate_seq_num(msg);
        auto bs = msg.get_int(FIXTag::BeginSeqNo);
        auto es = msg.get_int(FIXTag::EndSeqNo);
        if (bs.ok() && es.ok()) resend_messages(static_cast<u64>(*bs), static_cast<u64>(*es));
    }

    void handle_sequence_reset(const FIXMessage& msg) noexcept {
        auto ns = msg.get_int(FIXTag::NewSeqNo);
        if (ns.ok()) seq_num_in_ = static_cast<u64>(*ns) + 1;
    }

    void handle_reject(const FIXMessage& msg) noexcept { validate_seq_num(msg); }

    bool validate_seq_num(const FIXMessage& msg) noexcept {
        auto sq = msg.get_int(FIXTag::MsgSeqNum);
        if (!sq.ok()) return false;
        u64 seq = static_cast<u64>(*sq);
        auto dup = msg.get_field(FIXTag::PossDupFlag);
        bool is_dup = dup.ok() && dup->view() == "Y"_v;
        if (!is_dup) seq_num_in_ = seq + 1;
        return true;
    }

    void store_sent_message(const FIXMessage& msg) noexcept {
        if (sent_messages_.length() >= 256) {
            for (u64 i = 1; i < sent_messages_.length(); i++)
                sent_messages_[i-1] = spp::move(sent_messages_[i]);
            sent_messages_.pop();
        }
        FIXMessage copy;
        copy.begin_string_ = String{msg.begin_string_};
        copy.msg_type_ = String{msg.msg_type_};
        for (u64 i = 0; i < msg.fields_.length(); i++)
            copy.fields_.push(FIXField::make(msg.fields_[i].tag_, String{msg.fields_[i].value_}));
        sent_messages_.push(spp::move(copy));
    }

    void resend_messages(u64 from_seq, u64 to_seq) noexcept {
        for (u64 i = 0; i < sent_messages_.length(); i++) {
            const FIXMessage& msg = sent_messages_[i];
            auto sq = msg.get_int(FIXTag::MsgSeqNum);
            if (sq.ok()) {
                u64 seq = static_cast<u64>(*sq);
                if (seq >= from_seq && seq <= to_seq) {
                    FIXMessage resend;
                    resend.begin_string_ = String{msg.begin_string_};
                    resend.msg_type_ = String{msg.msg_type_};
                    for (u64 j = 0; j < msg.fields_.length(); j++)
                        resend.fields_.push(FIXField::make(msg.fields_[j].tag_, String{msg.fields_[j].value_}));
                    resend.fields_.push(FIXField::make(FIXTag::PossDupFlag, "Y"_v));
                    String wire = resend.encode();
                    sock_send(socket_fd_, wire.view());
                }
            }
        }
    }

    FIXMessage build_logon(u32 hi, bool reset_seq = false) noexcept {
        FIXMessage msg{FIXMsgType::Logon};
        msg.fields_.push(FIXField::make_int(FIXTag::EncryptMethod, 0));
        msg.fields_.push(FIXField::make_int(FIXTag::HeartBtInt, static_cast<i64>(hi)));
        if (!username_.empty()) msg.fields_.push(FIXField::make(FIXTag::Username, String{username_}));
        if (!password_.empty()) msg.fields_.push(FIXField::make(FIXTag::Password, String{password_}));
        if (reset_seq) msg.fields_.push(FIXField::make_int(FIXTag::ResetSeqNumFlag, 1));
        return msg;
    }

    FIXMessage build_logout(String_View reason = ""_v) noexcept {
        FIXMessage msg{FIXMsgType::Logout};
        if (!reason.empty()) msg.fields_.push(FIXField::make(FIXTag::Text, String{reason}));
        return msg;
    }
};

// =========================================================================
// FIXGateway — callback-based async FIX gateway
// =========================================================================
struct FIXGateway {
    FIXSession session_;

    void (*on_fill_)(const FillEvent& fill) = null;
    void (*on_order_update_)(String_View cl_ord_id, OrderStatus status, String_View message) = null;
    void (*on_reject_)(String_View cl_ord_id, String_View reason) = null;
    void (*on_disconnect_)(String_View reason) = null;
    void (*on_error_)(String_View error) = null;

    FIXGateway() = default;

    bool start(String_View host, u16 port, String_View sender_id, String_View target_id) noexcept {
        session_.sender_comp_id_ = String{sender_id};
        session_.target_comp_id_ = String{target_id};
        if (!session_.connect(host, port)) { if (on_error_) on_error_("Failed to connect"_v); return false; }
        if (!session_.logon(30)) { if (on_error_) on_error_("Logon failed"_v); session_.disconnect(); return false; }
        return true;
    }

    void stop() noexcept { session_.logout(); session_.disconnect(); }

    u64 poll() noexcept {
        if (session_.state_ != FIXSessionState::LoggedIn) {
            if (session_.state_ == FIXSessionState::Disconnected) {
                if (on_disconnect_) on_disconnect_("Session disconnected"_v);
            }
            return 0;
        }

        u64 count = 0;
        for (;;) {
            auto msg = session_.recv(0);
            if (!msg.ok()) break;
            count++;
            session_.handle_message(*msg);

            if (msg->msg_type() == FIXMsgType::ExecutionReport) {
                auto fill = session_.parse_execution_report(*msg);
                if (fill.ok() && on_fill_) on_fill_(*fill);
                if (!fill.ok()) {
                    auto os = msg->get_field(FIXTag::OrdStatus);
                    auto ci = msg->get_field(FIXTag::ClOrdID);
                    if (os.ok() && ci.ok()) {
                        OrderStatus st = OrderStatus::New;
                        String_View sv = os->view();
                        if (sv == FIXOrdStatus::Filled) st = OrderStatus::Filled;
                        else if (sv == FIXOrdStatus::Partial) st = OrderStatus::Partial;
                        else if (sv == FIXOrdStatus::Cancelled) st = OrderStatus::Cancelled;
                        else if (sv == FIXOrdStatus::Rejected) st = OrderStatus::Rejected;
                        if (on_order_update_) on_order_update_(ci->view(), st, os->view());
                    }
                }
            } else if (msg->msg_type() == FIXMsgType::Reject) {
                auto reason = msg->get_field(FIXTag::Text);
                if (on_reject_) on_reject_(""_v, reason.ok() ? reason->view() : "Order rejected"_v);
            } else if (msg->msg_type() == FIXMsgType::OrderCancelReject) {
                auto reason = msg->get_field(FIXTag::Text);
                auto ci = msg->get_field(FIXTag::ClOrdID);
                if (on_reject_) on_reject_(ci.ok() ? ci->view() : ""_v, reason.ok() ? reason->view() : "Cancel rejected"_v);
            } else if (msg->msg_type() == FIXMsgType::Logout) {
                auto reason = msg->get_field(FIXTag::Text);
                if (on_disconnect_) on_disconnect_(reason.ok() ? reason->view() : "Session logout"_v);
            }
        }
        return count;
    }

    String send_order(String_View symbol, OrderSide side, OrderType type,
                      f64 quantity, f64 price = 0.0) noexcept {
        return session_.send_new_order(symbol, side, type, quantity, price);
    }

    bool cancel_order(String_View cl_ord_id, String_View symbol, OrderSide side) noexcept {
        return session_.send_cancel_order(cl_ord_id, symbol, side);
    }

    bool is_connected() const noexcept { return session_.state_ == FIXSessionState::LoggedIn; }
    void heartbeat() noexcept { session_.heartbeat(); }
};

} // namespace spp::quant::execution

// =========================================================================
// SPP reflection records (at global scope)
// =========================================================================
SPP_NAMED_RECORD(::spp::quant::execution::FIXField, "FIXField",
                 SPP_FIELD(tag_), SPP_FIELD(value_));

SPP_NAMED_RECORD(::spp::quant::execution::FIXMessage, "FIXMessage",
                 SPP_FIELD(begin_string_), SPP_FIELD(msg_type_));
