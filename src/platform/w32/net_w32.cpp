
#include <spp/io/net.h>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace spp::Net {

[[nodiscard]] static String_View wsa_error_code(int err) noexcept {

    constexpr u64 buffer_size = 256;
    static thread_local char buffer[buffer_size] = {};

    u32 written = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, null,
                                 err, LANG_USER_DEFAULT, buffer, buffer_size, null);
    assert(written + 1 <= buffer_size);

    if(written <= 1) {
        int written2 =
            Libc::snprintf(reinterpret_cast<u8*>(buffer), buffer_size, "WinSock Error: %d", err);
        assert(written2 > 0 && written2 + 1 <= buffer_size);
        return String_View{reinterpret_cast<const u8*>(buffer), static_cast<u64>(written2)};
    }

    return String_View{reinterpret_cast<const u8*>(buffer), static_cast<u64>(written - 1)};
}

[[nodiscard]] static String_View wsa_error() noexcept {
    int err = WSAGetLastError();
    if(err == 0) return String_View{};
    return wsa_error_code(err);
}

struct WSA_Startup {
    WSA_Startup() noexcept {
        WSADATA wsa;
        int err = WSAStartup(MAKEWORD(2, 2), &wsa);
        if(err != 0) {
            warn("Failed to startup winsock: %", wsa_error_code(err));
        }
    }
};

static WSA_Startup g_wsa_startup;

static_assert(sizeof(sockaddr_in) == 16);
static_assert(alignof(sockaddr_in) == 4);

Address::Address(String_View address_, u16 port) noexcept {
    Region(R) {
        auto address = address_.terminate<Mregion<R>>();

        sockaddr_in& sockaddr_ = *reinterpret_cast<sockaddr_in*>(sockaddr_storage);
        sockaddr_ = {};
        sockaddr_.sin_family = AF_INET;
        sockaddr_.sin_port = htons(port);

        int ret = inet_pton(AF_INET, reinterpret_cast<const char*>(address.data()),
                            &sockaddr_.sin_addr.s_addr);

        if(ret == 0) {
            warn("Failed to create address: Invalid address.");
        }
        if(ret == -1) {
            warn("Failed to create address: %", wsa_error());
        }
    }
}

Address::Address(u16 port) noexcept {
    sockaddr_in& sockaddr_ = *reinterpret_cast<sockaddr_in*>(sockaddr_storage);
    sockaddr_ = {};
    sockaddr_.sin_family = AF_INET;
    sockaddr_.sin_port = htons(port);
    sockaddr_.sin_addr.s_addr = INADDR_ANY;
}

Udp::Udp() noexcept {

    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(socket == INVALID_SOCKET) {
        die("Failed to open socket: %", wsa_error());
    }
    handle_ = IO::socket_handle(static_cast<uptr>(socket));

    u_long imode = 1;
    if(ioctlsocket(socket, FIONBIO, &imode) != NO_ERROR) {
        die("Failed to set socket nonblocked: %", wsa_error());
    }
}

Udp::~Udp() noexcept {
    static_cast<void>(IO::close_result(handle_));
}

Udp::Udp(Udp&& src) noexcept {
    handle_ = src.handle_;
    src.handle_.invalidate();
}

Udp& Udp::operator=(Udp&& src) noexcept {
    if(this == &src) return *this;
    static_cast<void>(IO::close_result(handle_));
    handle_ = src.handle_;
    src.handle_.invalidate();
    return *this;
}

void Udp::bind(Address address) noexcept {
    SOCKET socket = static_cast<SOCKET>(handle_.native);

    if(::bind(socket, reinterpret_cast<SOCKADDR*>(address.sockaddr_storage), sizeof(sockaddr_in)) ==
       SOCKET_ERROR) {
        die("Failed to bind socket: %", wsa_error());
    }
}

[[nodiscard]] Result<Udp::Data, String_View> Udp::recv_result(Packet& in) noexcept {

    sockaddr_in src;
    SOCKET socket = static_cast<SOCKET>(handle_.native);

    i32 src_len = sizeof(src);
    i32 ret = recvfrom(socket, reinterpret_cast<char*>(in.data()), static_cast<i32>(in.length()), 0,
                       reinterpret_cast<SOCKADDR*>(&src), &src_len);
    if(ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if(err == WSAEWOULDBLOCK) {
            return Result<Data, String_View>::err("would_block"_v);
        }
        return Result<Data, String_View>::err(wsa_error_code(err));
    }
    if(ret < 0) {
        return Result<Data, String_View>::err("recv_failed"_v);
    }

    Address retaddr;
    *reinterpret_cast<sockaddr_in*>(retaddr.sockaddr_storage) = src;

    return Result<Data, String_View>::ok(Data{static_cast<u64>(ret), spp::move(retaddr)});
}

[[nodiscard]] u64 Udp::send(Address address, const Packet& out, u64 length) noexcept {
    SOCKET socket = static_cast<SOCKET>(handle_.native);

    i32 ret =
        sendto(socket, reinterpret_cast<const char*>(out.data()), static_cast<i32>(length), 0,
               reinterpret_cast<const SOCKADDR*>(address.sockaddr_storage), sizeof(sockaddr_in));
    if(ret == SOCKET_ERROR) {
        warn("Failed send packet: %", wsa_error());
    }
    return ret;
}

} // namespace spp::Net
