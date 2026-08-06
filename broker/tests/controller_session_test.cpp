#include "mango_overlay/broker/controller_session.hpp"
#include "mango_overlay/protocol/controller.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <cstdio>
#include <variant>

using mango_overlay::broker::ControllerSession;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ControllerGetStatus;
using mango_overlay::protocol::ControllerSetEnabled;
using mango_overlay::protocol::ControllerSetRequireApproval;
using mango_overlay::protocol::ControllerStatus;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::decode_controller_status;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_controller_request;
using mango_overlay::protocol::encode_packet;
using mango_overlay::scene::OverlayPolicy;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::Visibility;

namespace {

template <typename Request>
std::vector<std::uint8_t> request_packet(
    MessageType type,
    std::uint64_t request_id,
    const Request& request)
{
    const auto payload = encode_controller_request(request);
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, type, 0, request_id },
        ByteView { payload.data(), payload.size() });
}

const ControllerStatus* response_status(
    const mango_overlay::broker::ControllerSessionResponse& response,
    ControllerStatus& storage)
{
    const auto decoded = decode_packet(
        ByteView { response.packet.data(), response.packet.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    if (packet == nullptr || packet->header.message_type != MessageType::controller_status) {
        return nullptr;
    }
    const auto status = decode_controller_status(packet->payload);
    const auto* value = std::get_if<ControllerStatus>(&status);
    if (value == nullptr) {
        return nullptr;
    }
    storage = *value;
    return &storage;
}

} // namespace

int main()
{
    SceneStore scenes;
    scenes.register_provider(
        1,
        ProviderIdentity {
            "dev.example.provider",
            "primary",
            "Provider",
            1280,
            800,
            Visibility::always,
        });
    bool persistence_succeeds = true;
    std::vector<OverlayPolicy> persisted;
    ControllerSession session(
        scenes,
        ProtocolVersion { 1, 0 },
        [&](const OverlayPolicy& policy) {
            persisted.push_back(policy);
            return persistence_succeeds;
        });

    ControllerStatus storage {};
    const auto get = request_packet(
        MessageType::controller_get_status, 10, ControllerGetStatus {});
    const auto initial = session.process(ByteView { get.data(), get.size() });
    const auto* initial_status = response_status(initial, storage);
    if (initial_status == nullptr || !initial_status->enabled
        || initial_status->applications.size() != 1
        || initial_status->applications[0].active_instances != 1) {
        std::fputs("controller did not report the live provider\n", stderr);
        return 1;
    }

    const auto disable = request_packet(
        MessageType::controller_set_enabled, 11, ControllerSetEnabled { false });
    const auto disabled = session.process(ByteView { disable.data(), disable.size() });
    const auto* disabled_status = response_status(disabled, storage);
    if (disabled_status == nullptr || disabled_status->enabled
        || persisted.size() != 1 || persisted[0].enabled
        || !scenes.snapshot()->providers.empty()) {
        std::fputs("controller did not atomically disable provider canvases\n", stderr);
        return 1;
    }

    persistence_succeeds = false;
    const auto require = request_packet(
        MessageType::controller_set_require_approval,
        12,
        ControllerSetRequireApproval { true });
    const auto failed = session.process(ByteView { require.data(), require.size() });
    const auto decoded_failed = decode_packet(
        ByteView { failed.packet.data(), failed.packet.size() });
    const auto* failed_packet = std::get_if<DecodedPacketView>(&decoded_failed);
    if (failed_packet == nullptr
        || failed_packet->header.message_type != MessageType::error
        || scenes.policy().require_approval) {
        std::fputs("failed controller persistence changed live policy\n", stderr);
        return 1;
    }
    return 0;
}
