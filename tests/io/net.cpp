
#include "test.h"

#include <spp/io/net.h>

i32 main() {
    Test test{"net"_v};
    {
        Net::Address addr{"127.0.0.1"_v, 25565};
        Net::Udp udp;

        udp.bind(addr);
        Net::Packet packet;
        u64 i = 0;
        for(char c : "Hello"_v) {
            packet[i++] = c;
        }
        static_cast<void>(udp.send(addr, packet, 5));

        Thread::sleep(100);

        auto data = udp.recv_result(packet);
        assert(data.ok());
        assert(data.unwrap().length == 5);
        info("%", String_View{packet.data(), data.unwrap().length});

        auto data_compat = udp.recv(packet);
        assert(!data_compat.ok());
    }
    return 0;
}
