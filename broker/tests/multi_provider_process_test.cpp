#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>
#include <signal.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ConnectionRole;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::Hello;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_hello;
using mango_overlay::protocol::encode_packet;

namespace {

std::vector<std::uint8_t> hello_packet(std::uint64_t request_id)
{
    const auto payload = encode_hello(Hello {
        ConnectionRole::provider,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 0 },
        0,
        0,
        "concurrency.test/1.0",
    });
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, request_id },
        ByteView { payload.data(), payload.size() });
}

std::vector<std::uint8_t> registration_packet(
    const std::string& application_id,
    std::uint64_t request_id)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        builder.CreateString(application_id),
        builder.CreateString("primary"),
        builder.CreateString(application_id),
        1280,
        800,
        MangoOverlay::Wire::Visibility::GameOnly);
    builder.Finish(registration);
    return encode_packet(
        PacketHeader {
            ProtocolVersion { 1, 0 },
            MessageType::register_provider,
            0,
            request_id,
        },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

bool receive_type(int descriptor, MessageType expected, std::uint64_t request_id)
{
    std::array<std::uint8_t, 4096> response {};
    const auto size = recv(descriptor, response.data(), response.size(), 0);
    if (size <= 0) {
        return false;
    }
    const auto decoded = decode_packet(
        ByteView { response.data(), static_cast<std::size_t>(size) });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    return packet != nullptr && packet->header.message_type == expected
        && packet->header.request_id == request_id;
}

bool start_provider(
    int descriptor,
    const std::string& application_id,
    std::uint64_t first_request_id)
{
    const auto hello = hello_packet(first_request_id);
    const auto registration = registration_packet(application_id, first_request_id + 1);
    return send(descriptor, hello.data(), hello.size(), MSG_NOSIGNAL)
            == static_cast<ssize_t>(hello.size())
        && send(descriptor, registration.data(), registration.size(), MSG_NOSIGNAL)
            == static_cast<ssize_t>(registration.size())
        && receive_type(descriptor, MessageType::hello_accepted, first_request_id)
        && receive_type(
            descriptor,
            MessageType::provider_registered,
            first_request_id + 1);
}

int connect_client(const sockaddr_un& address, socklen_t address_size)
{
    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    const timeval timeout { 2, 0 };
    if (descriptor < 0
        || setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0
        || connect(
               descriptor,
               reinterpret_cast<const sockaddr*>(&address),
               address_size)
            != 0) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        return -1;
    }
    return descriptor;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("broker executable path is required\n", stderr);
        return 1;
    }

    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::string socket_name = "mango-overlay-concurrency-test-"
        + std::to_string(getpid());
    std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + socket_name.size());
    if (listener < 0
        || bind(listener, reinterpret_cast<sockaddr*>(&address), address_size) != 0
        || listen(listener, 2) != 0) {
        std::perror("socket/bind/listen");
        return 1;
    }

    const pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        close(listener);
        return 1;
    }
    if (child == 0) {
        if (fcntl(listener, F_SETFD, 0) != 0) {
            _exit(126);
        }
        const std::string descriptor = std::to_string(listener);
        execl(argv[1], argv[1], "--listen-fd", descriptor.c_str(), nullptr);
        _exit(127);
    }
    close(listener);

    const int first = connect_client(address, address_size);
    const bool first_started = first >= 0
        && start_provider(first, "concurrency.first", 10);
    const int second = connect_client(address, address_size);
    const bool second_started = second >= 0
        && start_provider(second, "concurrency.second", 20);

    if (first >= 0) {
        close(first);
    }
    if (second >= 0) {
        close(second);
    }
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);

    if (!first_started || !second_started) {
        std::fputs("broker did not serve two live providers concurrently\n", stderr);
        return 1;
    }
    return 0;
}
