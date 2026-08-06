#include "mango_overlay/broker/snapshot_encoder.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdio>
#include <memory>
#include <variant>
#include <vector>

using mango_overlay::broker::encode_scene_change_packets;
using mango_overlay::broker::encode_snapshot_packets;
using mango_overlay::broker::OutboundPacket;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::decode_packet;
using mango_overlay::scene::Color;
using mango_overlay::scene::Element;
using mango_overlay::scene::ImageElement;
using mango_overlay::scene::ImageResource;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::ProviderScene;
using mango_overlay::scene::RectangleElement;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneChange;
using mango_overlay::scene::SceneChangeKind;
using mango_overlay::scene::SceneSnapshot;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

namespace {

const DecodedPacketView* decode(
    const OutboundPacket& outbound,
    mango_overlay::protocol::DecodeResult& storage)
{
    const auto& bytes = outbound.bytes;
    storage = decode_packet(ByteView { bytes.data(), bytes.size() });
    return std::get_if<DecodedPacketView>(&storage);
}

} // namespace

int main()
{
    SceneStore store;
    store.register_provider(
        1,
        ProviderIdentity {
            "snapshot.test", "primary", "Snapshot Test", 1280, 800, Visibility::always });
    store.commit(
        1,
        SceneTransaction {
            1,
            {
                UpsertElement { Element {
                    1,
                    2,
                    TextElement {
                        Vec2 { 12.0F, 24.0F },
                        "Snapshot text",
                        22.0F,
                        Color { 1.0F, 0.5F, 0.25F, 1.0F },
                    },
                } },
                UpsertElement { Element {
                    2,
                    1,
                    RectangleElement {
                        Vec2 { 40.0F, 50.0F },
                        Vec2 { 100.0F, 60.0F },
                        8.0F,
                        Color { 0.1F, 0.2F, 0.3F, 0.8F },
                    },
                } },
            },
        });

    const auto snapshot = store.snapshot();
    const auto packets = encode_snapshot_packets(
        *snapshot, ProtocolVersion { 1, 0 }, 77);
    if (packets.size() != 3) {
        std::fputs("one-provider snapshot was not encoded as begin/provider/end\n", stderr);
        return 1;
    }

    mango_overlay::protocol::DecodeResult decoded_begin;
    mango_overlay::protocol::DecodeResult decoded_provider;
    mango_overlay::protocol::DecodeResult decoded_end;
    const auto* begin = decode(packets[0], decoded_begin);
    const auto* provider = decode(packets[1], decoded_provider);
    const auto* end = decode(packets[2], decoded_end);
    if (begin == nullptr || provider == nullptr || end == nullptr
        || begin->header.message_type != MessageType::scene_snapshot_begin
        || provider->header.message_type != MessageType::scene_snapshot_provider
        || end->header.message_type != MessageType::scene_snapshot_end
        || begin->header.request_id != 77 || provider->header.request_id != 77
        || end->header.request_id != 77) {
        std::fputs("snapshot packet framing is incorrect\n", stderr);
        return 1;
    }

    flatbuffers::Verifier provider_verifier(provider->payload.data, provider->payload.size);
    if (!provider_verifier.VerifyBuffer<MangoOverlay::Wire::SceneSnapshotProvider>(nullptr)) {
        std::fputs("snapshot provider payload is invalid\n", stderr);
        return 1;
    }
    const auto* wire_provider
        = flatbuffers::GetRoot<MangoOverlay::Wire::SceneSnapshotProvider>(
            provider->payload.data);
    if (wire_provider->revision() != 2 || wire_provider->provider() == nullptr
        || wire_provider->provider()->application_id()->str() != "snapshot.test"
        || wire_provider->provider()->requested_visibility()
            != MangoOverlay::Wire::Visibility::Always
        || wire_provider->provider()->elements() == nullptr
        || wire_provider->provider()->elements()->size() != 2) {
        std::fputs("snapshot provider payload lost scene data\n", stderr);
        return 1;
    }

    auto large_resource = std::make_shared<const ImageResource>(ImageResource {
        100,
        std::vector<std::uint8_t>(
            mango_overlay::broker::inline_resource_threshold + 1, 0x5a),
        mango_overlay::resource::DecodedImage {
            mango_overlay::resource::ImageFormat::png,
            1,
            1,
            { 0 },
            { 255, 255, 255, 255 },
        },
    });
    auto resource_provider = std::make_shared<ProviderScene>(*snapshot->providers[0]);
    resource_provider->elements.push_back(Element {
        3,
        3,
        ImageElement {
            Vec2 { 200.0F, 100.0F },
            Vec2 { 64.0F, 64.0F },
            100,
            Color { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    });
    resource_provider->resources.push_back(large_resource);
    const auto resource_packets = encode_snapshot_packets(
        SceneSnapshot { 3, { resource_provider } },
        ProtocolVersion { 1, 0 },
        88);
    mango_overlay::protocol::DecodeResult decoded_resource;
    mango_overlay::protocol::DecodeResult decoded_resource_provider;
    const auto* resource_packet = resource_packets.size() == 4
        ? decode(resource_packets[1], decoded_resource)
        : nullptr;
    const auto* resource_provider_packet = resource_packets.size() == 4
        ? decode(resource_packets[2], decoded_resource_provider)
        : nullptr;
    if (resource_packet == nullptr || resource_provider_packet == nullptr
        || resource_packet->header.message_type != MessageType::resource_available
        || (resource_packet->header.flags
               & mango_overlay::protocol::packet_flag_file_descriptor)
            == 0
        || resource_packets[1].attachment != large_resource
        || resource_provider_packet->header.message_type
            != MessageType::scene_snapshot_provider) {
        std::fputs("large resource was not attached before its provider scene\n", stderr);
        return 1;
    }

    const auto resource_update_packets = encode_scene_change_packets(
        SceneChange {
            4,
            SceneChangeKind::upsert,
            resource_provider->identity,
            resource_provider,
        },
        ProtocolVersion { 1, 0 });
    mango_overlay::protocol::DecodeResult decoded_update_resource;
    mango_overlay::protocol::DecodeResult decoded_update_provider;
    const auto* update_resource = resource_update_packets.size() == 2
        ? decode(resource_update_packets[0], decoded_update_resource)
        : nullptr;
    const auto* update_provider = resource_update_packets.size() == 2
        ? decode(resource_update_packets[1], decoded_update_provider)
        : nullptr;
    if (update_resource == nullptr || update_provider == nullptr
        || update_resource->header.message_type != MessageType::resource_available
        || update_provider->header.message_type
            != MessageType::provider_scene_updated
        || resource_update_packets[0].attachment != large_resource) {
        std::fputs("live resource was not attached before its provider update\n", stderr);
        return 1;
    }

    const auto changes = store.changes_after(1);
    if (changes.history_gap || changes.changes.size() != 1) {
        std::fputs("could not obtain update for encoder test\n", stderr);
        return 1;
    }
    const auto update_packets = encode_scene_change_packets(
        changes.changes[0], ProtocolVersion { 1, 0 });
    mango_overlay::protocol::DecodeResult decoded_update;
    const auto* update = update_packets.size() == 1
        ? decode(update_packets[0], decoded_update)
        : nullptr;
    if (update == nullptr
        || update->header.message_type != MessageType::provider_scene_updated
        || update->header.request_id != 0) {
        std::fputs("live provider update packet is incorrect\n", stderr);
        return 1;
    }

    store.disconnect(1);
    const auto removal = store.changes_after(2);
    const auto removal_packets = encode_scene_change_packets(
        removal.changes[0], ProtocolVersion { 1, 0 });
    mango_overlay::protocol::DecodeResult decoded_removal;
    const auto* removed = removal_packets.size() == 1
        ? decode(removal_packets[0], decoded_removal)
        : nullptr;
    if (removed == nullptr
        || removed->header.message_type != MessageType::provider_scene_removed) {
        std::fputs("provider removal packet is incorrect\n", stderr);
        return 1;
    }
    return 0;
}
