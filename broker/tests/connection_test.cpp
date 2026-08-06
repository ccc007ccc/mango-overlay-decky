#include "mango_overlay/broker/connection.hpp"
#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <variant>

using mango_overlay::broker::ConnectionResult;
using mango_overlay::broker::serve_initial_handshake;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ConnectionRole;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::Hello;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::ServerHandshake;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_hello;
using mango_overlay::protocol::encode_packet;

int main()
{
    std::array<int, 2> sockets {};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) != 0) {
        std::perror("socketpair");
        return 1;
    }

    const auto hello_payload = encode_hello(Hello {
        ConnectionRole::renderer,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 0 },
        0,
        0x01,
        "renderer.test/1.0",
    });
    const auto request = encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, 99 },
        ByteView { hello_payload.data(), hello_payload.size() });
    if (send(sockets[0], request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
        std::perror("send");
        return 1;
    }

    const auto result = serve_initial_handshake(
        sockets[1],
        ServerHandshake {
            ProtocolVersion { 1, 0 },
            ProtocolVersion { 1, 0 },
            0x01,
            "mango-overlayd/0.1",
        });
    if (result.result != ConnectionResult::accepted || !result.accepted.has_value()
        || result.accepted->role != ConnectionRole::renderer
        || result.accepted->version.major != 1
        || result.accepted->version.minor != 0
        || result.accepted->capabilities != 0x01) {
        std::fputs("broker did not accept a valid seqpacket handshake\n", stderr);
        return 1;
    }

    std::array<std::uint8_t, 4096> response {};
    const auto response_size = recv(sockets[0], response.data(), response.size(), 0);
    close(sockets[0]);
    close(sockets[1]);
    if (response_size <= 0) {
        std::perror("recv");
        return 1;
    }

    const auto decoded = decode_packet(ByteView {
        response.data(), static_cast<std::size_t>(response_size) });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    if (packet == nullptr || packet->header.message_type != MessageType::hello_accepted
        || packet->header.request_id != 99) {
        std::fputs("seqpacket response was not the correlated HelloAccepted packet\n", stderr);
        return 1;
    }

    return 0;
}
