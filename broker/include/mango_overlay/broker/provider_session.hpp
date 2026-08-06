#pragma once

#include "mango_overlay/protocol/error.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <cstdint>
#include <vector>

namespace mango_overlay::broker {

struct SessionResponse {
    std::vector<std::uint8_t> packet;
    bool close_after_send;
};

class ProviderSession {
public:
    ProviderSession(
        scene::SceneStore& scenes,
        scene::ConnectionId connection,
        protocol::ProtocolVersion version);
    ~ProviderSession();

    ProviderSession(const ProviderSession&) = delete;
    ProviderSession& operator=(const ProviderSession&) = delete;

    SessionResponse process(protocol::ByteView packet, int attachment_fd = -1);
    bool registered() const { return registered_; }

private:
    SessionResponse reject(
        std::uint64_t request_id,
        protocol::ErrorCode code,
        const char* message);

    scene::SceneStore& scenes_;
    scene::ConnectionId connection_;
    protocol::ProtocolVersion version_;
    bool registered_ = false;
    std::uint8_t protocol_errors_ = 0;
};

} // namespace mango_overlay::broker
