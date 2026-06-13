// spp-quant MCP Server — Claude Code agent interface for Binance quant trading.
//
// Build: SPP_TLS=1 make mcp
// Usage: ./spp_mcp [--host api.binance.com] [--key API_KEY] [--secret SECRET]
//
// Communicates via JSON-RPC 2.0 over stdin/stdout. Designed for Claude Code
// to drive as an MCP server (configure in .mcp.json).
//
// Logs go to stderr; stdout is the MCP protocol channel.

#include <spp/core/base.h>
#include <spp/io/handle.h>
#include <spp/io/files.h>
#include <cstdlib>
#include <cstring>
#include <spp/concurrency/thread.h>

#include <binance/client.h>
#include <binance/models.h>
#include <binance/clock.h>
#include <okx/client.h>

#include <mcp/codec/jsonrpc_codec.h>
#include <mcp/handler.h>

using namespace spp;

// Minimal config parsing from environment / CLI
struct Server_Config {
    String_View host = "api.binance.com"_v;
    u16 port = 443;
    String<Mdefault> api_key;
    String<Mdefault> api_secret;
    // OKX is optional — only enabled if all three credentials are
    // provided (API key, secret, passphrase). Env vars OKX_API_KEY /
    // OKX_SECRET / OKX_PASSPHRASE.
    String<Mdefault> okx_api_key;
    String<Mdefault> okx_api_secret;
    String<Mdefault> okx_passphrase;
    String_View okx_host = "www.okx.com"_v;
    bool okx_simulated = false;
};

static Server_Config parse_config(int argc, char** argv) noexcept {
    Server_Config cfg;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        u64 alen = (u64)std::strlen(a);
        String_View arg{(const u8*)a, alen};
        if (arg == "--host"_v && i + 1 < argc) {
            const char* v = argv[++i];
            cfg.host = String_View{(const u8*)v, (u64)std::strlen(v)};
        } else if (arg == "--port"_v && i + 1 < argc) {
            cfg.port = (u16)std::strtoll(argv[++i], null, 10);
        } else if (arg == "--key"_v && i + 1 < argc) {
            const char* v = argv[++i];
            String_View sv{(const u8*)v, (u64)std::strlen(v)};
            cfg.api_key = sv.string<Mdefault>();
        } else if (arg == "--secret"_v && i + 1 < argc) {
            const char* v = argv[++i];
            String_View sv{(const u8*)v, (u64)std::strlen(v)};
            cfg.api_secret = sv.string<Mdefault>();
        }
    }

    if (cfg.api_key.length() == 0) {
        const char* env = getenv("BINANCE_API_KEY");
        if (env) {
            String_View sv{(const u8*)env, (u64)std::strlen(env)};
            cfg.api_key = sv.string<Mdefault>();
        }
    }
    if (cfg.api_secret.length() == 0) {
        const char* env = getenv("BINANCE_SECRET");
        if (env) {
            String_View sv{(const u8*)env, (u64)std::strlen(env)};
            cfg.api_secret = sv.string<Mdefault>();
        }
    }

    auto load_env = [](const char* name, String<Mdefault>& out) {
        const char* env = getenv(name);
        if (env) {
            String_View sv{(const u8*)env, (u64)std::strlen(env)};
            out = sv.string<Mdefault>();
        }
    };
    load_env("OKX_API_KEY",    cfg.okx_api_key);
    load_env("OKX_SECRET",     cfg.okx_api_secret);
    load_env("OKX_PASSPHRASE", cfg.okx_passphrase);
    if (const char* env = getenv("OKX_HOST")) {
        cfg.okx_host = String_View{(const u8*)env, (u64)std::strlen(env)};
    }
    if (const char* env = getenv("OKX_SIMULATED")) {
        cfg.okx_simulated = (env[0] == '1' || env[0] == 't' || env[0] == 'T');
    }
    return cfg;
}

// Log message to stderr (does NOT touch stdout — that's the MCP channel).
static void log_stderr(String_View msg) noexcept {
    IO::Handle stderr_h = IO::file_handle(2);
    Slice<const u8> data{msg.data(), msg.length()};
    IO::write_some_result(stderr_h, data);
    const u8 nl = '\n';
    IO::write_some_result(stderr_h, Slice<const u8>{&nl, 1});
}

int main(int argc, char** argv) {
    auto cfg = parse_config(argc, argv);

    log_stderr("[spp_mcp] starting..."_v);

    // Setup Binance client
    App::Binance::Client_Config bcfg;
    bcfg.host = cfg.host;
    bcfg.port = cfg.port;
    bcfg.api_key = cfg.api_key.view();
    bcfg.api_secret = cfg.api_secret.view();

    bool has_auth = cfg.api_key.length() > 0 && cfg.api_secret.length() > 0;
    if (!has_auth) {
        log_stderr("[spp_mcp] WARNING: no API key/secret — trading tools disabled; market data + analysis OK"_v);
    }

    App::Binance::Client client(spp::move(bcfg));
    auto conn = client.connect();
    if (!conn.ok()) {
        log_stderr("[spp_mcp] WARNING: Binance connection failed — tools will return errors"_v);
    } else {
        log_stderr("[spp_mcp] connected to Binance"_v);
        auto sync = client.sync_time_if_stale();
        if (sync.ok()) {
            log_stderr("[spp_mcp] time synced"_v);
        }
    }

    // Optional OKX client — opened only when all three credentials are
    // present.  Otherwise the okx_* MCP tools return `no_okx_client`.
    Opt<App::Okx::Client> okx_client;
    if (cfg.okx_api_key.length() > 0 &&
        cfg.okx_api_secret.length() > 0 &&
        cfg.okx_passphrase.length() > 0) {
        App::Okx::Client_Config ocfg;
        ocfg.host = cfg.okx_host;
        ocfg.api_key    = cfg.okx_api_key.view();
        ocfg.api_secret = cfg.okx_api_secret.view();
        ocfg.passphrase = cfg.okx_passphrase.view();
        ocfg.simulated_trading = cfg.okx_simulated;
        okx_client.emplace(spp::move(ocfg));
        auto oconn = okx_client->connect();
        if (oconn.ok()) {
            log_stderr("[spp_mcp] connected to OKX"_v);
        } else {
            log_stderr("[spp_mcp] WARNING: OKX connection failed — okx_* tools will return errors"_v);
        }
    } else {
        log_stderr("[spp_mcp] OKX disabled (set OKX_API_KEY/OKX_SECRET/OKX_PASSPHRASE to enable)"_v);
    }

    // Setup MCP handler
    mcp::MCP_Handler handler;
    handler.set_client(&client);
    if (okx_client.ok()) {
        handler.set_okx_client(&*okx_client);
    }

    // Open stdio handles
    IO::Handle stdin_h  = IO::file_handle(0);
    IO::Handle stdout_h = IO::file_handle(1);

    // Main poll loop — single-threaded, blocking reads on stdin.
    // We use a simple blocking read since we only care about stdin.
    // For a production server that also monitors WebSocket streams,
    // we'd use IO::poll_any_result with multiple targets.
    log_stderr("[spp_mcp] entering main loop"_v);

    bool running = true;
    while (running) {
        auto msg_json = mcp::read_message(stdin_h);
        if (!msg_json.ok()) {
            String_View e = msg_json.unwrap_err();
            if (e == "would_block"_v) {
                Thread::sleep(50);
                continue;
            }
            // EOF or read error → exit
            log_stderr("[spp_mcp] stdin closed, exiting"_v);
            break;
        }

        auto& json_str = msg_json.unwrap();
        if (json_str.length() == 0) continue;

        // Parse JSON-RPC message
        auto parsed = mcp::parse_message(json_str);
        if (!parsed.ok()) {
            auto err_resp = mcp::make_error_response(0, mcp::MCP_PARSE_ERROR, "parse_error"_v);
            mcp::write_response(stdout_h, err_resp);
            continue;
        }

        auto& msg = parsed.unwrap();
        auto response = handler.process(msg);

        // notifications/initialized has no response
        if (response.length() > 0) {
            auto wr = mcp::write_response(stdout_h, response);
            if (!wr.ok()) {
                log_stderr("[spp_mcp] write error, exiting"_v);
                break;
            }
        }
    }

    client.close();
    log_stderr("[spp_mcp] shutdown complete"_v);
    return 0;
}
