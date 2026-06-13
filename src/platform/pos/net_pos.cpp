
#include <spp/io/net.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>

namespace spp::Net {

Tcp_Client::Tcp_Client(Tcp_Client&& src) noexcept {
    handle_ = src.handle_;
    src.handle_.invalidate();
}

Tcp_Client& Tcp_Client::operator=(Tcp_Client&& src) noexcept {
    if(this == &src) return *this;
    close();
    handle_ = src.handle_;
    src.handle_.invalidate();
    return *this;
}

[[nodiscard]] Result<u64, String_View>
Tcp_Client::connect_result(String_View host, u16 port) noexcept {
    if(handle_.valid()) return Result<u64, String_View>::err("already_connected"_v);

    // getaddrinfo wants a NUL-terminated host; copy locally.
    char host_buf[256];
    if(host.length() >= sizeof(host_buf)) {
        return Result<u64, String_View>::err("host_too_long"_v);
    }
    for(u64 i = 0; i < host.length(); i++) host_buf[i] = static_cast<char>(host[i]);
    host_buf[host.length()] = '\0';

    char port_buf[16];
    {
        i32 wrote = Libc::snprintf(reinterpret_cast<u8*>(port_buf), sizeof(port_buf),
                                   "%u", static_cast<unsigned>(port));
        if(wrote <= 0) return Result<u64, String_View>::err("port_format"_v);
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    int rc = getaddrinfo(host_buf, port_buf, &hints, &res);
    if(rc != 0 || !res) {
        return Result<u64, String_View>::err("dns_failed"_v);
    }

    int fd = -1;
    for(addrinfo* it = res; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if(fd < 0) continue;
        if(::connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if(fd < 0) return Result<u64, String_View>::err("connect_failed"_v);

    handle_ = IO::socket_handle(static_cast<uptr>(fd));
    return Result<u64, String_View>::ok(0);
}

[[nodiscard]] Result<u64, String_View> Tcp_Client::send_all_result(Slice<const u8> in) noexcept {
    if(!handle_.valid()) return Result<u64, String_View>::err("invalid_handle"_v);
    int fd = static_cast<int>(handle_.native);
    u64 sent = 0;
    while(sent < in.length()) {
        // MSG_NOSIGNAL on Linux suppresses SIGPIPE; macOS doesn't expose it but
        // an EPIPE return is acceptable for our error path.
#ifdef SPP_OS_LINUX
        i64 n = ::send(fd, in.data() + sent, in.length() - sent, MSG_NOSIGNAL);
#else
        i64 n = ::send(fd, in.data() + sent, in.length() - sent, 0);
#endif
        if(n < 0) {
            if(errno == EINTR) continue;
            return Result<u64, String_View>::err(Log::sys_error());
        }
        if(n == 0) return Result<u64, String_View>::err("send_eof"_v);
        sent += static_cast<u64>(n);
    }
    return Result<u64, String_View>::ok(spp::move(sent));
}

[[nodiscard]] Result<u64, String_View> Tcp_Client::recv_result(Slice<u8> out) noexcept {
    if(!handle_.valid()) return Result<u64, String_View>::err("invalid_handle"_v);
    int fd = static_cast<int>(handle_.native);
    for(;;) {
        i64 n = ::recv(fd, out.data(), out.length(), 0);
        if(n < 0) {
            if(errno == EINTR) continue;
            return Result<u64, String_View>::err(Log::sys_error());
        }
        return Result<u64, String_View>::ok(static_cast<u64>(n));
    }
}

[[nodiscard]] Result<u64, String_View> Tcp_Client::recv_exact_result(Slice<u8> out) noexcept {
    if(!handle_.valid()) return Result<u64, String_View>::err("invalid_handle"_v);
    int fd = static_cast<int>(handle_.native);
    u64 got = 0;
    while(got < out.length()) {
        i64 n = ::recv(fd, out.data() + got, out.length() - got, 0);
        if(n < 0) {
            if(errno == EINTR) continue;
            return Result<u64, String_View>::err(Log::sys_error());
        }
        if(n == 0) return Result<u64, String_View>::err("short_read"_v);
        got += static_cast<u64>(n);
    }
    return Result<u64, String_View>::ok(spp::move(got));
}

void Tcp_Client::close() noexcept {
    if(!handle_.valid()) return;
    static_cast<void>(IO::close_result(handle_));
}

Address::Address(String_View address, u16 port) noexcept {
    sockaddr_ = {};
    sockaddr_.sin_family = AF_INET;
    sockaddr_.sin_port = htons(port);

    if(inet_pton(AF_INET, reinterpret_cast<const char*>(address.data()),
                 &sockaddr_.sin_addr.s_addr) != 1) {
        die("Failed to create address: %", Log::sys_error());
    }
}

Address::Address(u16 port) noexcept {
    sockaddr_ = {};
    sockaddr_.sin_family = AF_INET;
    sockaddr_.sin_port = htons(port);
    sockaddr_.sin_addr.s_addr = INADDR_ANY;
}

Udp::Udp() noexcept {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        die("Failed to open socket: %", Log::sys_error());
    }
    handle_ = IO::socket_handle(static_cast<uptr>(fd));
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
    int fd = static_cast<int>(handle_.native);
    if(::bind(fd, reinterpret_cast<const sockaddr*>(&address.sockaddr_), sizeof(sockaddr_in)) < 0) {
        die("Failed to bind socket: %", Log::sys_error());
    }
}

[[nodiscard]] Result<Udp::Data, String_View> Udp::recv_result(Packet& in) noexcept {

    Address src;
    socklen_t src_len = sizeof(src.sockaddr_);
    int fd = static_cast<int>(handle_.native);

    i64 ret = ::recvfrom(fd, in.begin(), in.capacity, MSG_DONTWAIT | MSG_TRUNC,
                         reinterpret_cast<sockaddr*>(&src.sockaddr_), &src_len);

    if(ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return Result<Data, String_View>::err("would_block"_v);
    }
    if(ret < 0) {
        return Result<Data, String_View>::err(Log::sys_error());
    }
    if(static_cast<u64>(ret) > in.length()) {
        return Result<Data, String_View>::err("packet_truncated"_v);
    }

    return Result<Data, String_View>::ok(Data{static_cast<u64>(ret), spp::move(src)});
}

[[nodiscard]] u64 Udp::send(Address address, const Packet& out, u64 length) noexcept {
    int fd = static_cast<int>(handle_.native);

    i64 ret = ::sendto(fd, out.data(), length, 0,
                       reinterpret_cast<const sockaddr*>(&address.sockaddr_), sizeof(sockaddr_in));
    if(ret == -1) {
        die("Failed send packet: %", Log::sys_error());
    }
    return ret;
}

} // namespace spp::Net
