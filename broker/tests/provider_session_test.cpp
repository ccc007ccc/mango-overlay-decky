#include "mango_overlay/broker/provider_session.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>

#include <cstdio>
#include <variant>

using mango_overlay::broker::ProviderSession;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_packet;
using mango_overlay::scene::SceneStore;

int main()
{
    SceneStore scenes;
    ProviderSession session(scenes, 100, ProtocolVersion { 1, 0 });

    flatbuffers::FlatBufferBuilder builder;
    const auto application_id = builder.CreateString("example.telemetry");
    const auto instance_id = builder.CreateString("primary");
    const auto display_name = builder.CreateString("Example Telemetry");
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        application_id,
        instance_id,
        display_name,
        1280,
        800,
        MangoOverlay::Wire::Visibility::GameOnly);
    builder.Finish(registration);
    const auto request = encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::register_provider, 0, 55 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });

    const auto response = session.process(ByteView { request.data(), request.size() });
    if (response.close_after_send) {
        std::fputs("valid provider registration closed the session\n", stderr);
        return 1;
    }

    const auto decoded_response = decode_packet(
        ByteView { response.packet.data(), response.packet.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded_response);
    if (packet == nullptr || packet->header.message_type != MessageType::provider_registered
        || packet->header.request_id != 55) {
        std::fputs("provider session returned an invalid registration response\n", stderr);
        return 1;
    }
    flatbuffers::Verifier verifier(packet->payload.data, packet->payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ProviderRegistered>(nullptr)
        || flatbuffers::GetRoot<MangoOverlay::Wire::ProviderRegistered>(packet->payload.data)
                ->scene_revision()
            != 1) {
        std::fputs("provider registration response has the wrong scene revision\n", stderr);
        return 1;
    }

    const auto snapshot = scenes.snapshot();
    if (snapshot->providers.size() != 1
        || snapshot->providers[0]->identity.application_id != "example.telemetry"
        || snapshot->providers[0]->identity.instance_id != "primary") {
        std::fputs("registered provider is missing from the scene store\n", stderr);
        return 1;
    }

    return 0;
}
