#include "test.h"

#include <spp/io/net.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Local loopback round-trip. We open a server-side listener manually (since
// SPP only exposes a TCP client, not a server primitive yet), accept on a
// background thread, and exercise connect/send/recv on the client.
i32 main() {
    Test test{"empty"_v};

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    assert(srv >= 0);

    int opt = 1;
    static_cast<void>(setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // kernel picks
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    assert(getsockname(srv, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0);
    u16 port = ntohs(bound.sin_port);

    assert(listen(srv, 1) == 0);

    auto server_task = Thread::spawn([srv]() -> i32 {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int conn = accept(srv, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if(conn < 0) return 1;
        u8 buf[16]{};
        ssize_t n = read(conn, buf, sizeof(buf));
        if(n <= 0) {
            close(conn);
            return 2;
        }
        // Echo with a fixed prefix so we can assert the exchange directly.
        const char* reply = "ECHO:";
        if(write(conn, reply, 5) != 5) {
            close(conn);
            return 3;
        }
        if(write(conn, buf, n) != n) {
            close(conn);
            return 4;
        }
        close(conn);
        return 0;
    });

    Net::Tcp_Client client;
    auto connected = client.connect_result("127.0.0.1"_v, port);
    assert(connected.ok());
    assert(client.valid());

    u8 msg[] = {'P', 'I', 'N', 'G'};
    auto sent = client.send_all_result(Slice<const u8>{msg, 4});
    assert(sent.ok());
    assert(sent.unwrap() == 4);

    u8 reply[9]{};
    auto got = client.recv_exact_result(Slice<u8>{reply, 9});
    assert(got.ok());
    assert(got.unwrap() == 9);
    assert(reply[0] == 'E' && reply[1] == 'C' && reply[2] == 'H' && reply[3] == 'O');
    assert(reply[4] == ':');
    assert(reply[5] == 'P' && reply[6] == 'I' && reply[7] == 'N' && reply[8] == 'G');

    client.close();
    assert(!client.valid());

    i32 server_rc = server_task->block();
    assert(server_rc == 0);

    close(srv);
    return 0;
}
