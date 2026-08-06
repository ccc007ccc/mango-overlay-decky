#pragma once

#include "mango_overlay/broker/snapshot_encoder.hpp"
#include "mango_overlay/protocol/error.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace mango_overlay::broker {

struct RendererSessionResponse {
    std::vector<OutboundPacket> packets;
    bool close_after_send;
};

class RendererSession {
public:
    RendererSession(
        scene::SceneStore& scenes,
        protocol::ProtocolVersion version);

    RendererSessionResponse process(protocol::ByteView packet);
    RendererSessionResponse wait_for_updates(std::chrono::milliseconds timeout);

private:
    RendererSessionResponse reject(
        std::uint64_t request_id,
        protocol::ErrorCode code,
        const char* message);

    scene::SceneStore& scenes_;
    protocol::ProtocolVersion version_;
    std::uint64_t revision_ = 0;
    std::uint8_t protocol_errors_ = 0;
    bool subscribed_ = false;
};

} // namespace mango_overlay::broker
