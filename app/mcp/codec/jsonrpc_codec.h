#pragma once

#include <spp/core/base.h>
#include <spp/core/opt.h>
#include <spp/core/result.h>
#include <spp/containers/string0.h>
#include <spp/containers/string1.h>
#include <spp/containers/vec.h>
#include <spp/io/handle.h>
#include <spp/io/files.h>
#include <spp/reflection/json.h>

// JSON-RPC 2.0 codec for MCP (Model Context Protocol) over stdio.
//
// SPP's JSON parser is static/reflective (no dynamic tree), so we parse
// incoming messages into a struct where all possible fields are Opt<>.

namespace spp::mcp {

// --- Error codes ---
constexpr i64 MCP_PARSE_ERROR      = -32700;
constexpr i64 MCP_INVALID_REQUEST  = -32600;
constexpr i64 MCP_METHOD_NOT_FOUND = -32601;
constexpr i64 MCP_INVALID_PARAMS   = -32602;
constexpr i64 MCP_INTERNAL_ERROR   = -32603;
constexpr i64 MCP_TOOL_NOT_FOUND   = -32001;

struct MCP_Error {
    i64 code = 0;
    String<Mdefault> message;
};

// All fields are Opt<> — parse once, dispatch by which are populated.
struct Message_In {
    Opt<String<Mdefault>> jsonrpc;
    Opt<String<Mdefault>> method;
    Opt<u64> id;
    Opt<String<Mdefault>> params;  // raw JSON, deferred tool-parse
    Opt<String<Mdefault>> result;
    Opt<MCP_Error> error;
};

struct Result_Out {
    String<Mdefault> jsonrpc;
    u64 id = 0;
    Opt<String<Mdefault>> result;
    Opt<MCP_Error> error;
};

enum class Msg_Type : u8 { request, response, notification, invalid };

[[nodiscard]] inline Msg_Type classify(const Message_In& m) noexcept {
    if (m.method.ok() && m.id.ok()) return Msg_Type::request;
    if (m.result.ok() || m.error.ok()) return Msg_Type::response;
    if (m.method.ok()) return Msg_Type::notification;
    return Msg_Type::invalid;
}

// === JSON response builders ===
// Build MCP-compliant JSON-RPC 2.0 responses as String<Mdefault>.
// We use a simple string builder approach compatible with spp's String API.

[[nodiscard]] inline String<Mdefault> make_init_response(u64 id) noexcept {
    char buf[2048];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%llu,"
        "\"result\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"spp-quant-mcp\",\"version\":\"1.0.0\"}}}",
        (unsigned long long)id);
    String<Mdefault> s((u64)(n > 0 ? n + 1 : 1));
    if (n > 0) {
        s.set_length((u64)n);
        Libc::memcpy(s.data(), buf, (u64)n);
    }
    s.data()[s.length()] = 0;
    return s;
}

[[nodiscard]] inline String<Mdefault> make_tools_list_response(u64 id, String_View tools_json) noexcept {
    // tools_json should be the comma-joined tool objects: {"name":"t1",...},{"name":"t2",...}
    char buf[4096];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"tools\":[%.*s]}}",
        (unsigned long long)id, (i32)tools_json.length(), tools_json.data());
    String<Mdefault> s((u64)(n > 0 ? n + 1 : 1));
    if (n > 0) {
        s.set_length((u64)n);
        Libc::memcpy(s.data(), buf, (u64)n);
    }
    s.data()[s.length()] = 0;
    return s;
}

[[nodiscard]] inline String<Mdefault> make_tool_result(u64 id, String_View text_json) noexcept {
    // text_json must be a valid JSON string (already escaped)
    char buf[32768];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%llu,"
        "\"result\":{\"content\":[{\"type\":\"text\",\"text\":%.*s}]}}",
        (unsigned long long)id, (i32)text_json.length(), text_json.data());
    String<Mdefault> s((u64)(n > 0 ? n + 1 : 1));
    if (n > 0) {
        s.set_length((u64)n);
        Libc::memcpy(s.data(), buf, (u64)n);
    }
    s.data()[s.length()] = 0;
    return s;
}

[[nodiscard]] inline String<Mdefault> make_error_response(u64 id, i64 code, String_View msg) noexcept {
    char buf[4096];
    i32 n = Libc::snprintf((u8*)buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%llu,"
        "\"error\":{\"code\":%lld,\"message\":\"%.*s\"}}",
        (unsigned long long)id, (long long)code, (i32)msg.length(), msg.data());
    String<Mdefault> s((u64)(n > 0 ? n + 1 : 1));
    if (n > 0) {
        s.set_length((u64)n);
        Libc::memcpy(s.data(), buf, (u64)n);
    }
    s.data()[s.length()] = 0;
    return s;
}

// === I/O ===

// Read one JSON-RPC line from stdin (newline-delimited framing).
[[nodiscard]] inline Result<String<Mdefault>, String_View>
read_message(IO::Handle stdin_h) noexcept {
    constexpr u64 CAP = 65536;
    u8 raw[CAP];
    u64 total = 0;

    while (total < CAP) {
        Slice<u8> dest{raw + total, CAP - total};
        auto rr = IO::read_some_result(stdin_h, dest);
        if (!rr.ok()) {
            String_View e = rr.unwrap_err();
            if (e == "would_block"_v) continue;
            return Result<String<Mdefault>, String_View>::err(spp::move(e));
        }
        u64 nr = rr.unwrap();
        if (nr == 0) break;
        total += nr;
        // Check for newline in newly-read bytes
        for (u64 j = total - nr; j < total; j++) {
            if (raw[j] == '\n') {
                u64 end = j;
                if (end > 0 && raw[end - 1] == '\r') end--;
                u64 len = end;
                String<Mdefault> s(len + 1);
                if (len > 0) {
                    s.set_length(len);
                    Libc::memcpy(s.data(), raw, len);
                }
                s.data()[len] = 0;
                return Result<String<Mdefault>, String_View>::ok(spp::move(s));
            }
        }
    }
    return Result<String<Mdefault>, String_View>::err("mcp_read_error"_v);
}

// Parse incoming JSON into Message_In.
[[nodiscard]] inline Result<Message_In, String_View>
parse_message(const String<Mdefault>& json_str) noexcept {
    String_View sv(json_str.data(), json_str.length());
    return Json::parse_result<Message_In>(sv);
}

// Write a response string to stdout, followed by newline.
[[nodiscard]] inline Result<u64, String_View>
write_response(IO::Handle stdout_h, const String<Mdefault>& out) noexcept {
    Slice<const u8> data{out.data(), out.length()};
    auto rr = IO::write_some_result(stdout_h, data);
    if (!rr.ok()) return rr;
    const u8 nl = '\n';
    return IO::write_some_result(stdout_h, Slice<const u8>{&nl, 1});
}

} // namespace spp::mcp

namespace spp {

SPP_NAMED_RECORD(mcp::MCP_Error, "MCP_Error",
    SPP_FIELD(code), SPP_FIELD(message));

SPP_NAMED_RECORD(mcp::Message_In, "MCP_Message_In",
    SPP_FIELD(jsonrpc), SPP_FIELD(method), SPP_FIELD(id),
    SPP_FIELD(params), SPP_FIELD(result), SPP_FIELD(error));

SPP_NAMED_RECORD(mcp::Result_Out, "MCP_Result_Out",
    SPP_FIELD(jsonrpc), SPP_FIELD(id), SPP_FIELD(result), SPP_FIELD(error));

} // namespace spp
