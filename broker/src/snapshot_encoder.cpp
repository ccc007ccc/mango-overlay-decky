#include "mango_overlay/broker/snapshot_encoder.hpp"
#include "mango_overlay/wire/scene_codec.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <stdexcept>

namespace mango_overlay::broker {

namespace {

std::vector<std::uint8_t> make_packet(
    protocol::ProtocolVersion version,
    protocol::MessageType type,
    std::uint64_t request_id,
    flatbuffers::FlatBufferBuilder& builder)
{
    return protocol::encode_packet(
        protocol::PacketHeader { version, type, 0, request_id },
        protocol::ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

OutboundPacket encode_resource(
    const scene::ProviderScene& provider,
    const std::shared_ptr<const scene::ImageResource>& resource,
    std::uint64_t revision,
    protocol::ProtocolVersion version,
    std::uint64_t request_id)
{
    if (resource == nullptr || resource->encoded.empty()) {
        throw std::invalid_argument("provider scene contains an invalid resource");
    }
    const bool attached = resource->encoded.size() > inline_resource_threshold;
    flatbuffers::FlatBufferBuilder builder;
    flatbuffers::Offset<flatbuffers::Vector<std::uint8_t>> inline_data;
    if (!attached) {
        inline_data = builder.CreateVector(resource->encoded);
    }
    const auto available = MangoOverlay::Wire::CreateResourceAvailable(
        builder,
        revision,
        builder.CreateString(provider.identity.application_id),
        builder.CreateString(provider.identity.instance_id),
        resource->id,
        static_cast<std::uint32_t>(resource->encoded.size()),
        inline_data);
    builder.Finish(available);
    auto bytes = protocol::encode_packet(
        protocol::PacketHeader {
            version,
            protocol::MessageType::resource_available,
            static_cast<std::uint16_t>(
                attached ? protocol::packet_flag_file_descriptor : 0),
            request_id,
        },
        protocol::ByteView { builder.GetBufferPointer(), builder.GetSize() });
    return { std::move(bytes), attached ? resource : nullptr };
}

std::vector<std::uint8_t> encode_snapshot_begin(
    const scene::SceneSnapshot& snapshot,
    protocol::ProtocolVersion version,
    std::uint64_t request_id)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto begin = MangoOverlay::Wire::CreateSceneSnapshotBegin(
        builder,
        snapshot.revision,
        static_cast<std::uint32_t>(snapshot.providers.size()));
    builder.Finish(begin);
    return make_packet(
        version, protocol::MessageType::scene_snapshot_begin, request_id, builder);
}

std::vector<std::uint8_t> encode_snapshot_provider(
    const scene::ProviderScene& provider,
    std::uint64_t revision,
    protocol::ProtocolVersion version,
    std::uint64_t request_id)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto wire_provider = wire::encode_provider(builder, provider);
    const auto message = MangoOverlay::Wire::CreateSceneSnapshotProvider(
        builder, revision, wire_provider);
    builder.Finish(message);
    return make_packet(
        version, protocol::MessageType::scene_snapshot_provider, request_id, builder);
}

std::vector<std::uint8_t> encode_snapshot_end(
    std::uint64_t revision,
    protocol::ProtocolVersion version,
    std::uint64_t request_id)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto end = MangoOverlay::Wire::CreateSceneSnapshotEnd(builder, revision);
    builder.Finish(end);
    return make_packet(
        version, protocol::MessageType::scene_snapshot_end, request_id, builder);
}

} // namespace

std::vector<OutboundPacket> encode_snapshot_packets(
    const scene::SceneSnapshot& snapshot,
    protocol::ProtocolVersion version,
    std::uint64_t request_id)
{
    std::size_t resource_count = 0;
    for (const auto& provider : snapshot.providers) {
        if (provider != nullptr) {
            resource_count += provider->resources.size();
        }
    }
    std::vector<OutboundPacket> packets;
    packets.reserve(snapshot.providers.size() + resource_count + 2);
    packets.push_back({ encode_snapshot_begin(snapshot, version, request_id), nullptr });
    for (const auto& provider : snapshot.providers) {
        if (provider == nullptr) {
            throw std::invalid_argument("snapshot contains a null provider scene");
        }
        for (const auto& resource : provider->resources) {
            packets.push_back(encode_resource(
                *provider, resource, snapshot.revision, version, request_id));
        }
        packets.push_back({
            encode_snapshot_provider(
                *provider, snapshot.revision, version, request_id),
            nullptr,
        });
    }
    packets.push_back({
        encode_snapshot_end(snapshot.revision, version, request_id), nullptr });
    return packets;
}

std::vector<OutboundPacket> encode_scene_change_packets(
    const scene::SceneChange& change,
    protocol::ProtocolVersion version)
{
    std::vector<OutboundPacket> packets;
    if (change.kind == scene::SceneChangeKind::upsert) {
        if (change.provider == nullptr) {
            throw std::invalid_argument("provider update has no scene");
        }
        packets.reserve(change.provider->resources.size() + 1);
        for (const auto& resource : change.provider->resources) {
            packets.push_back(encode_resource(
                *change.provider, resource, change.revision, version, 0));
        }
        flatbuffers::FlatBufferBuilder builder;
        const auto provider = wire::encode_provider(builder, *change.provider);
        const auto update = MangoOverlay::Wire::CreateProviderSceneUpdated(
            builder, change.revision, provider);
        builder.Finish(update);
        packets.push_back({
            make_packet(
                version, protocol::MessageType::provider_scene_updated, 0, builder),
            nullptr,
        });
    } else {
        flatbuffers::FlatBufferBuilder builder;
        const auto removal = MangoOverlay::Wire::CreateProviderSceneRemoved(
            builder,
            change.revision,
            builder.CreateString(change.identity.application_id),
            builder.CreateString(change.identity.instance_id));
        builder.Finish(removal);
        packets.push_back({
            make_packet(
                version, protocol::MessageType::provider_scene_removed, 0, builder),
            nullptr,
        });
    }
    return packets;
}

} // namespace mango_overlay::broker
