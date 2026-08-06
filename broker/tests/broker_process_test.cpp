#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>

#include <array>
#include <cerrno>
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

std::vector<std::uint8_t> registration_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const auto application_id = builder.CreateString("process.test");
    const auto instance_id = builder.CreateString("primary");
    const auto display_name = builder.CreateString("Process Test");
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        application_id,
        instance_id,
        display_name,
        1280,
        800,
        MangoOverlay::Wire::Visibility::GameOnly);
    builder.Finish(registration);
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::register_provider, 0, 8 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

std::vector<std::uint8_t> transaction_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Vec2 position(24.0F, 36.0F);
    const MangoOverlay::Wire::Color color(0.9F, 0.8F, 0.2F, 1.0F);
    const auto text = MangoOverlay::Wire::CreateTextElement(
        builder,
        &position,
        builder.CreateString("Process scene"),
        20.0F,
        &color);
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
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::scene_transaction, 0, 9 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("broker executable path is required\n", stderr);
        return 1;
    }

    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::string socket_name = "mango-overlay-process-test-" + std::to_string(getpid());
    std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + socket_name.size());
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), address_size) != 0
        || listen(listener, 1) != 0) {
        std::perror("bind/listen");
        close(listener);
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
        execl(argv[1], argv[1], "--listen-fd", descriptor.c_str(), "--once", nullptr);
        _exit(127);
    }
    close(listener);

    const int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (client < 0 || connect(client, reinterpret_cast<sockaddr*>(&address), address_size) != 0) {
        std::perror("connect");
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        return 1;
    }

    const auto hello_payload = encode_hello(Hello {
        ConnectionRole::provider,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 0 },
        0,
        0,
        "process.test/1.0",
    });
    const auto request = encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, 7 },
        ByteView { hello_payload.data(), hello_payload.size() });
    if (send(client, request.data(), request.size(), MSG_NOSIGNAL)
        != static_cast<ssize_t>(request.size())) {
        std::perror("send");
        return 1;
    }
    const auto registration_request = registration_packet();
    if (send(client, registration_request.data(), registration_request.size(), MSG_NOSIGNAL)
        != static_cast<ssize_t>(registration_request.size())) {
        std::perror("send registration");
        return 1;
    }
    const auto transaction_request = transaction_packet();
    if (send(client, transaction_request.data(), transaction_request.size(), MSG_NOSIGNAL)
        != static_cast<ssize_t>(transaction_request.size())) {
        std::perror("send transaction");
        return 1;
    }

    std::array<std::uint8_t, 4096> handshake_response {};
    const auto handshake_response_size = recv(
        client, handshake_response.data(), handshake_response.size(), 0);
    std::array<std::uint8_t, 4096> registration_response {};
    const auto registration_response_size = recv(
        client, registration_response.data(), registration_response.size(), 0);
    std::array<std::uint8_t, 4096> transaction_response {};
    const auto transaction_response_size = recv(
        client, transaction_response.data(), transaction_response.size(), 0);
    close(client);
    int child_status = 0;
    waitpid(child, &child_status, 0);
    if (handshake_response_size <= 0 || registration_response_size <= 0
        || transaction_response_size <= 0
        || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        std::fputs("broker process did not complete one connection cleanly\n", stderr);
        return 1;
    }

    const auto decoded_handshake = decode_packet(ByteView {
        handshake_response.data(), static_cast<std::size_t>(handshake_response_size) });
    const auto* handshake_packet = std::get_if<DecodedPacketView>(&decoded_handshake);
    if (handshake_packet == nullptr
        || handshake_packet->header.message_type != MessageType::hello_accepted
        || handshake_packet->header.request_id != 7) {
        std::fputs("broker process returned an invalid handshake response\n", stderr);
        return 1;
    }

    const auto decoded_registration = decode_packet(ByteView {
        registration_response.data(), static_cast<std::size_t>(registration_response_size) });
    const auto* registration_packet_response = std::get_if<DecodedPacketView>(
        &decoded_registration);
    if (registration_packet_response == nullptr
        || registration_packet_response->header.message_type != MessageType::provider_registered
        || registration_packet_response->header.request_id != 8) {
        std::fputs("broker process returned an invalid registration response\n", stderr);
        return 1;
    }

    const auto decoded_transaction = decode_packet(ByteView {
        transaction_response.data(), static_cast<std::size_t>(transaction_response_size) });
    const auto* transaction_packet_response = std::get_if<DecodedPacketView>(
        &decoded_transaction);
    if (transaction_packet_response == nullptr
        || transaction_packet_response->header.message_type
            != MessageType::transaction_committed
        || transaction_packet_response->header.request_id != 9) {
        std::fputs("broker process returned an invalid transaction response\n", stderr);
        return 1;
    }

    return 0;
}
