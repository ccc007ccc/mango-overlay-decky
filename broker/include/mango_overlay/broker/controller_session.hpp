#pragma once

#include "mango_overlay/protocol/error.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <cstdint>
#include <vector>

namespace mango_overlay::broker {

struct ControllerSessionResponse {
    std::vector<std::uint8_t> packet;
    bool close_after_send;
};

class ControllerSession {
public:
    ControllerSession(
        scene::SceneStore& scenes,
        protocol::ProtocolVersion version,
        scene::PolicyCommit persist_policy = {});

    ControllerSessionResponse process(protocol::ByteView packet);

private:
    ControllerSessionResponse reject(
        std::uint64_t request_id,
        protocol::ErrorCode code,
        const char* message);
    ControllerSessionResponse status(std::uint64_t request_id);

    scene::SceneStore& scenes_;
    protocol::ProtocolVersion version_;
    scene::PolicyCommit persist_policy_;
    std::uint8_t protocol_errors_ = 0;
};

} // namespace mango_overlay::broker
