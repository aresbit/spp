
#pragma once

#include <spp/core/base.h>
#include <spp/io/handle.h>

#if defined SPP_OS_LINUX || defined SPP_OS_MACOS 
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace spp::Net {

constexpr u16 default_port = 6969;
constexpr u64 min_transmissible_unit = 1472;

using Packet = Array<u8, min_transmissible_unit>;

struct Address {

    Address() = default;

    explicit Address(u16 port) noexcept;
    explicit Address(String_View address, u16 port) noexcept;

private:
#ifdef SPP_OS_WINDOWS
    alignas(4) u8 sockaddr_storage[16];
#else
    sockaddr_in sockaddr_;
#endif

    friend struct Udp;
};

// Blocking TCP client. Builds a connected socket via getaddrinfo, exposes a
// byte-stream API on top of IO::Handle, and is the layer protocol/http.h and
// protocol/websocket.h consume. TLS is intentionally not provided here — see
// docs/binance_integration.md for the integration shape.
struct Tcp_Client {
    Tcp_Client() noexcept = default;
    ~Tcp_Client() noexcept {
        close();
    }

    Tcp_Client(const Tcp_Client&) noexcept = delete;
    Tcp_Client& operator=(const Tcp_Client&) noexcept = delete;

    Tcp_Client(Tcp_Client&& src) noexcept;
    Tcp_Client& operator=(Tcp_Client&& src) noexcept;

    // Resolves host (DNS or numeric) and connects to port.
    [[nodiscard]] Result<u64, String_View> connect_result(String_View host, u16 port) noexcept;

    // Sends every byte or returns an error.
    [[nodiscard]] Result<u64, String_View> send_all_result(Slice<const u8> in) noexcept;

    // Reads up to out.length() bytes. Returns 0 on EOF.
    [[nodiscard]] Result<u64, String_View> recv_result(Slice<u8> out) noexcept;

    // Reads exactly out.length() bytes; reports short_read on EOF before that.
    [[nodiscard]] Result<u64, String_View> recv_exact_result(Slice<u8> out) noexcept;

    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return handle_.valid();
    }
    [[nodiscard]] const IO::Handle& handle() const noexcept {
        return handle_;
    }

private:
    IO::Handle handle_;
};

struct Udp {
    struct Data {
        u64 length;
        Address from;
    };

    Udp() noexcept;
    ~Udp() noexcept;

    Udp(const Udp& src) noexcept = delete;
    Udp& operator=(const Udp& src) noexcept = delete;

    Udp(Udp&& src) noexcept;
    Udp& operator=(Udp&& src) noexcept;

    void bind(Address address) noexcept;
    [[nodiscard]] u64 send(Address address, const Packet& out, u64 length) noexcept;
    [[nodiscard]] Result<Data, String_View> recv_result(Packet& in) noexcept;
    [[nodiscard]] inline Opt<Data> recv(Packet& in) noexcept {
        auto result = recv_result(in);
        if(!result.ok()) {
            return {};
        }
        return Opt<Data>{spp::move(result.unwrap())};
    }

private:
    IO::Handle handle_;
};

} // namespace spp::Net
