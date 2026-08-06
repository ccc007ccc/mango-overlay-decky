#pragma once

#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace mango_overlay::broker {

constexpr std::size_t inline_resource_threshold = 256 * 1024;

struct OutboundPacket {
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<const scene::ImageResource> attachment;
};

std::vector<OutboundPacket> encode_snapshot_packets(
    const scene::SceneSnapshot& snapshot,
    protocol::ProtocolVersion version,
    std::uint64_t request_id);

std::vector<OutboundPacket> encode_scene_change_packets(
    const scene::SceneChange& change,
    protocol::ProtocolVersion version);

} // namespace mango_overlay::broker
