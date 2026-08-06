#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/renderer.hpp"
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
using mango_overlay::protocol::RendererSubscription;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_hello;
using mango_overlay::protocol::encode_packet;
using mango_overlay::protocol::encode_renderer_subscription;

namespace {

std::vector<std::uint8_t> hello_packet(ConnectionRole role, std::uint64_t request_id)
{
    const auto payload = encode_hello(Hello {
        role,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 0 },
        role == ConnectionRole::renderer
            ? mango_overlay::protocol::capability::renderer_scene_stream
            : mango_overlay::protocol::capability::retained_scene_transactions,
        0,
        "renderer.process.test/1.0",
    });
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, request_id },
        ByteView { payload.data(), payload.size() });
}

std::vector<std::uint8_t> registration_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        builder.CreateString("renderer.process.provider"),
        builder.CreateString("primary"),
        builder.CreateString("Renderer Process Provider"),
        1280,
        800,
        MangoOverlay::Wire::Visibility::Always);
    builder.Finish(registration);
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::register_provider, 0, 2 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

std::vector<std::uint8_t> transaction_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Vec2 position(30.0F, 40.0F);
    const MangoOverlay::Wire::Color color(1.0F, 1.0F, 1.0F, 1.0F);
    const auto text = MangoOverlay::Wire::CreateTextElement(
        builder, &position, builder.CreateString("live"), 18.0F, &color);
    const auto element = MangoOverlay::Wire::CreateElement(
        builder,
        1,
        0,
        MangoOverlay::Wire::ElementContent::TextElement,
        text.Union());
    const auto upsert = MangoOverlay::Wire::CreateUpsertElement(builder, element);
    const auto mutation = MangoOverlay::Wire::CreateMutation(
        builder,
        MangoOverlay::Wire::MutationContent::UpsertElement,
        upsert.Union());
    const auto mutations = builder.CreateVector(&mutation, 1);
    const auto transaction = MangoOverlay::Wire::CreateSceneTransaction(builder, 1, mutations);
    builder.Finish(transaction);
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::scene_transaction, 0, 3 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

std::vector<std::uint8_t> subscription_packet(std::uint64_t request_id)
{
    const auto payload = encode_renderer_subscription(RendererSubscription { 0 });
    return encode_packet(
        PacketHeader {
            ProtocolVersion { 1, 0 },
            MessageType::renderer_subscribe,
            0,
            request_id,
        },
        ByteView { payload.data(), payload.size() });
}

bool send_all(int descriptor, const std::vector<std::uint8_t>& packet)
{
    return send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL)
        == static_cast<ssize_t>(packet.size());
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

bool start_renderer(
    int descriptor,
    std::uint64_t hello_request_id,
    std::uint64_t subscription_request_id)
{
    const auto hello = hello_packet(ConnectionRole::renderer, hello_request_id);
    const auto subscription = subscription_packet(subscription_request_id);
    return descriptor >= 0 && send_all(descriptor, hello)
        && receive_type(descriptor, MessageType::hello_accepted, hello_request_id)
        && send_all(descriptor, subscription)
        && receive_type(
            descriptor,
            MessageType::scene_snapshot_begin,
            subscription_request_id)
        && receive_type(
            descriptor,
            MessageType::scene_snapshot_provider,
            subscription_request_id)
        && receive_type(
            descriptor,
            MessageType::scene_snapshot_end,
            subscription_request_id);
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
    const std::string socket_name = "mango-overlay-renderer-test-"
        + std::to_string(getpid());
    std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + socket_name.size());
    if (listener < 0
        || bind(listener, reinterpret_cast<sockaddr*>(&address), address_size) != 0
        || listen(listener, 4) != 0) {
        std::perror("socket/bind/listen");
        return 1;
    }

    const pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
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

    const int provider = connect_client(address, address_size);
    const auto provider_hello = hello_packet(ConnectionRole::provider, 1);
    const auto registration = registration_packet();
    bool okay = provider >= 0 && send_all(provider, provider_hello)
        && receive_type(provider, MessageType::hello_accepted, 1)
        && send_all(provider, registration)
        && receive_type(provider, MessageType::provider_registered, 2);

    const int first_renderer = connect_client(address, address_size);
    const int second_renderer = connect_client(address, address_size);
    okay = okay && start_renderer(first_renderer, 101, 102)
        && start_renderer(second_renderer, 201, 202);

    const auto transaction = transaction_packet();
    okay = okay && send_all(provider, transaction)
        && receive_type(provider, MessageType::transaction_committed, 3)
        && receive_type(first_renderer, MessageType::provider_scene_updated, 0)
        && receive_type(second_renderer, MessageType::provider_scene_updated, 0);

    if (provider >= 0) {
        close(provider);
    }
    okay = okay
        && receive_type(first_renderer, MessageType::provider_scene_removed, 0)
        && receive_type(second_renderer, MessageType::provider_scene_removed, 0);

    if (first_renderer >= 0) {
        close(first_renderer);
    }
    if (second_renderer >= 0) {
        close(second_renderer);
    }
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);

    if (!okay) {
        std::fputs("broker did not stream provider scenes to two renderers\n", stderr);
        return 1;
    }
    return 0;
}
