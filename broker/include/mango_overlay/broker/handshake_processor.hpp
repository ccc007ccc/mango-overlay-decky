#pragma once

#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mango_overlay::broker {

struct AcceptedSession {
    protocol::ConnectionRole role;
    protocol::ProtocolVersion version;
    protocol::CapabilitySet capabilities;
};

struct InitialPacketResult {
    std::vector<std::uint8_t> response;
    std::optional<AcceptedSession> accepted;
};

InitialPacketResult process_initial_packet(
    protocol::ByteView packet,
    const protocol::ServerHandshake& server);

} // namespace mango_overlay::broker
