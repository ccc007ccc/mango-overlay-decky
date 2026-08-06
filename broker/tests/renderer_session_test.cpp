#include "mango_overlay/broker/renderer_session.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/renderer.hpp"
#include "mango_overlay/scene/store.hpp"

#include <chrono>
#include <cstdio>
#include <variant>

using mango_overlay::broker::RendererSession;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::RendererSubscription;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_packet;
using mango_overlay::protocol::encode_renderer_subscription;
using mango_overlay::scene::Color;
using mango_overlay::scene::Element;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneLimits;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

namespace {

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

SceneTransaction text_update(std::uint64_t transaction_id, const char* text)
{
    return SceneTransaction {
        transaction_id,
        { UpsertElement { Element {
            1,
            0,
            TextElement {
                Vec2 { 10.0F, 10.0F },
                text,
                18.0F,
                Color { 1.0F, 1.0F, 1.0F, 1.0F },
            },
        } } },
    };
}

MessageType packet_type(const mango_overlay::broker::OutboundPacket& outbound)
{
    const auto& bytes = outbound.bytes;
    const auto decoded = decode_packet(ByteView { bytes.data(), bytes.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    return packet == nullptr ? MessageType::error : packet->header.message_type;
}

} // namespace

int main()
{
    SceneLimits limits;
    limits.maximum_retained_changes = 1;
    SceneStore store(limits);
    store.register_provider(
        1,
        ProviderIdentity {
            "renderer.test", "primary", "Renderer Test", 1280, 800, Visibility::always });
    store.commit(1, text_update(1, "initial"));

    RendererSession session(store, ProtocolVersion { 1, 0 });
    const auto subscription = subscription_packet(90);
    const auto initial = session.process(
        ByteView { subscription.data(), subscription.size() });
    if (initial.close_after_send || initial.packets.size() != 3
        || packet_type(initial.packets[0]) != MessageType::scene_snapshot_begin
        || packet_type(initial.packets[1]) != MessageType::scene_snapshot_provider
        || packet_type(initial.packets[2]) != MessageType::scene_snapshot_end) {
        std::fputs("renderer did not receive an atomic initial snapshot\n", stderr);
        return 1;
    }

    store.commit(1, text_update(2, "live"));
    const auto live = session.wait_for_updates(std::chrono::milliseconds(0));
    if (live.close_after_send || live.packets.size() != 1
        || packet_type(live.packets[0]) != MessageType::provider_scene_updated) {
        std::fputs("renderer did not receive a live provider update\n", stderr);
        return 1;
    }

    store.commit(1, text_update(3, "missed one"));
    store.commit(1, text_update(4, "history gap"));
    const auto resynchronized = session.wait_for_updates(std::chrono::milliseconds(0));
    if (resynchronized.close_after_send || resynchronized.packets.size() != 3
        || packet_type(resynchronized.packets.front())
            != MessageType::scene_snapshot_begin
        || packet_type(resynchronized.packets.back()) != MessageType::scene_snapshot_end) {
        std::fputs("renderer history gap did not trigger a complete snapshot\n", stderr);
        return 1;
    }

    store.set_enabled(false);
    const auto disabled = session.wait_for_updates(std::chrono::milliseconds(0));
    if (disabled.close_after_send || disabled.packets.size() != 2
        || packet_type(disabled.packets.front())
            != MessageType::scene_snapshot_begin
        || packet_type(disabled.packets.back()) != MessageType::scene_snapshot_end) {
        std::fputs("renderer policy reset did not publish an empty snapshot\n", stderr);
        return 1;
    }

    store.set_enabled(true);
    const auto enabled = session.wait_for_updates(std::chrono::milliseconds(0));
    if (enabled.close_after_send || enabled.packets.size() != 3
        || packet_type(enabled.packets.front())
            != MessageType::scene_snapshot_begin
        || packet_type(enabled.packets.back()) != MessageType::scene_snapshot_end) {
        std::fputs("renderer policy reset did not restore the provider snapshot\n", stderr);
        return 1;
    }

    store.disconnect(1);
    const auto removed = session.wait_for_updates(std::chrono::milliseconds(0));
    if (removed.close_after_send || removed.packets.size() != 1
        || packet_type(removed.packets[0]) != MessageType::provider_scene_removed) {
        std::fputs("renderer did not receive provider removal\n", stderr);
        return 1;
    }
    return 0;
}
