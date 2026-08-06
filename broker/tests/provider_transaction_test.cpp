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
using mango_overlay::scene::TextElement;

namespace {

std::vector<std::uint8_t> registration_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        builder.CreateString("example.telemetry"),
        builder.CreateString("primary"),
        builder.CreateString("Example Telemetry"),
        1280,
        800,
        MangoOverlay::Wire::Visibility::GameOnly);
    builder.Finish(registration);
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::register_provider, 0, 1 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

std::vector<std::uint8_t> transaction_packet()
{
    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Vec2 position(20.0F, 40.0F);
    const MangoOverlay::Wire::Color color(0.1F, 0.2F, 0.3F, 1.0F);
    const auto text = MangoOverlay::Wire::CreateTextElement(
        builder, &position, builder.CreateString("Live value: 42"), 18.0F, &color);
    const auto element = MangoOverlay::Wire::CreateElement(
        builder,
        22,
        3,
        MangoOverlay::Wire::ElementContent::TextElement,
        text.Union());
    const auto upsert = MangoOverlay::Wire::CreateUpsertElement(builder, element);
    const auto mutation = MangoOverlay::Wire::CreateMutation(
        builder,
        MangoOverlay::Wire::MutationContent::UpsertElement,
        upsert.Union());
    const auto mutations = builder.CreateVector(&mutation, 1);
    const auto transaction = MangoOverlay::Wire::CreateSceneTransaction(builder, 9, mutations);
    builder.Finish(transaction);
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::scene_transaction, 0, 2 },
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

} // namespace

int main()
{
    SceneStore scenes;
    ProviderSession session(scenes, 200, ProtocolVersion { 1, 0 });

    const auto registration = registration_packet();
    if (session.process(ByteView { registration.data(), registration.size() }).close_after_send) {
        std::fputs("provider registration failed before transaction test\n", stderr);
        return 1;
    }

    const auto transaction = transaction_packet();
    const auto response = session.process(ByteView { transaction.data(), transaction.size() });
    if (response.close_after_send) {
        std::fputs("valid scene transaction closed the provider session\n", stderr);
        return 1;
    }
    const auto decoded_response = decode_packet(
        ByteView { response.packet.data(), response.packet.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded_response);
    if (packet == nullptr || packet->header.message_type != MessageType::transaction_committed
        || packet->header.request_id != 2) {
        std::fputs("provider session returned an invalid transaction response\n", stderr);
        return 1;
    }

    flatbuffers::Verifier verifier(packet->payload.data, packet->payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::TransactionCommitted>(nullptr)) {
        std::fputs("transaction response payload is invalid\n", stderr);
        return 1;
    }
    const auto* committed = flatbuffers::GetRoot<MangoOverlay::Wire::TransactionCommitted>(
        packet->payload.data);
    if (committed->transaction_id() != 9 || committed->scene_revision() != 2
        || committed->already_applied()) {
        std::fputs("transaction response values are incorrect\n", stderr);
        return 1;
    }

    const auto snapshot = scenes.snapshot();
    if (snapshot->providers.size() != 1 || snapshot->providers[0]->elements.size() != 1) {
        std::fputs("committed scene is missing from the snapshot\n", stderr);
        return 1;
    }
    const auto* text = std::get_if<TextElement>(&snapshot->providers[0]->elements[0].content);
    if (text == nullptr || text->text != "Live value: 42") {
        std::fputs("committed text element is incorrect\n", stderr);
        return 1;
    }

    return 0;
}
