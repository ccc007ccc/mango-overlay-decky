#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/renderer/scene_client.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>
#include <signal.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
using mango_overlay::renderer::SceneClient;
using mango_overlay::scene::TextElement;

namespace {

std::vector<std::uint8_t> hello_packet()
{
    const auto payload = encode_hello(Hello {
        ConnectionRole::provider,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 0 },
        mango_overlay::protocol::capability::retained_scene_transactions,
        0,
        "scene.client.test/1.0",
    });
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, 1 },
        ByteView { payload.data(), payload.size() });
}

std::vector<std::uint8_t> registration_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        builder.CreateString("scene.client.provider"),
        builder.CreateString("primary"),
        builder.CreateString("Scene Client Provider"),
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
    const MangoOverlay::Wire::Vec2 position(16.0F, 20.0F);
    const MangoOverlay::Wire::Color color(1.0F, 1.0F, 1.0F, 1.0F);
    const auto text = MangoOverlay::Wire::CreateTextElement(
        builder, &position, builder.CreateString("client live"), 18.0F, &color);
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

bool exchange(int descriptor, const std::vector<std::uint8_t>& request, MessageType expected)
{
    if (send(descriptor, request.data(), request.size(), MSG_NOSIGNAL)
        != static_cast<ssize_t>(request.size())) {
        return false;
    }
    std::array<std::uint8_t, 4096> response {};
    const auto size = recv(descriptor, response.data(), response.size(), 0);
    if (size <= 0) {
        return false;
    }
    const auto decoded = decode_packet(
        ByteView { response.data(), static_cast<std::size_t>(size) });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    return packet != nullptr && packet->header.message_type == expected;
}

template <typename Predicate>
bool wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

pid_t start_broker(
    const char* executable,
    const std::string& socket_path,
    const sockaddr_un& address,
    socklen_t address_size)
{
    unlink(socket_path.c_str());
    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0
        || bind(
               listener,
               reinterpret_cast<const sockaddr*>(&address),
               address_size)
            != 0
        || listen(listener, 4) != 0) {
        if (listener >= 0) {
            close(listener);
        }
        return -1;
    }

    const pid_t child = fork();
    if (child < 0) {
        close(listener);
        return -1;
    }
    if (child == 0) {
        if (fcntl(listener, F_SETFD, 0) != 0) {
            _exit(126);
        }
        const std::string descriptor = std::to_string(listener);
        execl(executable, executable, "--listen-fd", descriptor.c_str(), nullptr);
        _exit(127);
    }
    close(listener);
    return child;
}

int connect_provider(const sockaddr_un& address, socklen_t address_size)
{
    const int provider = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (provider < 0
        || connect(
               provider,
               reinterpret_cast<const sockaddr*>(&address),
               address_size)
            != 0
        || !exchange(provider, hello_packet(), MessageType::hello_accepted)
        || !exchange(provider, registration_packet(), MessageType::provider_registered)) {
        if (provider >= 0) {
            close(provider);
        }
        return -1;
    }
    return provider;
}

bool has_live_text(const SceneClient& client)
{
    const auto snapshot = client.snapshot();
    if (snapshot->providers.size() != 1
        || snapshot->providers[0]->elements.size() != 1) {
        return false;
    }
    const auto* text = std::get_if<TextElement>(
        &snapshot->providers[0]->elements[0].content);
    return text != nullptr && text->text == "client live";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("broker executable path is required\n", stderr);
        return 1;
    }

    const std::string socket_path = "/tmp/mango-overlay-scene-client-"
        + std::to_string(getpid()) + ".sock";
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
    pid_t child = start_broker(argv[1], socket_path, address, address_size);
    if (child < 0) {
        std::perror("start broker");
        return 1;
    }

    int provider = connect_provider(address, address_size);
    bool okay = provider >= 0;

    SceneClient client(socket_path);
    client.start();
    okay = okay && wait_until([&] {
        const auto snapshot = client.snapshot();
        return client.connected() && snapshot->revision >= 1
            && snapshot->providers.size() == 1;
    });

    okay = okay
        && exchange(provider, transaction_packet(), MessageType::transaction_committed)
        && wait_until([&] {
               return client.snapshot()->revision >= 2 && has_live_text(client);
           });

    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    child = -1;
    if (provider >= 0) {
        close(provider);
        provider = -1;
    }
    okay = okay && wait_until([&] { return !client.connected(); })
        && has_live_text(client);

    child = start_broker(argv[1], socket_path, address, address_size);
    okay = okay && child >= 0
        && wait_until([&] {
               const auto snapshot = client.snapshot();
               return client.connected() && snapshot->revision == 0
                   && snapshot->providers.empty();
           });
    provider = connect_provider(address, address_size);
    okay = okay && provider >= 0
        && exchange(provider, transaction_packet(), MessageType::transaction_committed)
        && wait_until([&] { return has_live_text(client); });

    client.stop();
    if (provider >= 0) {
        close(provider);
    }
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
    }
    unlink(socket_path.c_str());

    if (!okay) {
        std::fputs("renderer scene client did not survive a broker restart\n", stderr);
        return 1;
    }
    return 0;
}
