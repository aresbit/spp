
#include <spp/io/net.h>

#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>

namespace spp::Net {

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
