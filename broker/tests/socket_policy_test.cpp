#include "mango_overlay/broker/connection.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace {

std::chrono::milliseconds timeout_for(int descriptor, int option)
{
    timeval timeout {};
    socklen_t size = sizeof(timeout);
    if (getsockopt(descriptor, SOL_SOCKET, option, &timeout, &size) != 0) {
        return std::chrono::milliseconds(-1);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::seconds(timeout.tv_sec)
        + std::chrono::microseconds(timeout.tv_usec));
}

} // namespace

int main()
{
    int sockets[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        std::perror("socketpair");
        return 1;
    }

    const bool configured = mango_overlay::broker::configure_connection_socket(
        sockets[0], std::chrono::milliseconds(25), std::chrono::milliseconds(40));
    const auto receive_timeout = timeout_for(sockets[0], SO_RCVTIMEO);
    const auto send_timeout = timeout_for(sockets[0], SO_SNDTIMEO);
    const bool cleared = mango_overlay::broker::clear_receive_timeout(sockets[0]);
    const auto cleared_timeout = timeout_for(sockets[0], SO_RCVTIMEO);
    close(sockets[0]);
    close(sockets[1]);

    if (!configured || !cleared
        || receive_timeout < std::chrono::milliseconds(20)
        || send_timeout < std::chrono::milliseconds(35)
        || cleared_timeout != std::chrono::milliseconds(0)) {
        std::fputs("connection socket deadlines were not applied correctly\n", stderr);
        return 1;
    }
    return 0;
}
