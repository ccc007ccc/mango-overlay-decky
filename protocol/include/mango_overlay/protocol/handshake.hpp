#pragma once

#include "mango_overlay/protocol/packet.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace mango_overlay::protocol {

enum class ConnectionRole : std::uint8_t {
    provider = 1,
    renderer = 2,
    controller = 3,
};

using CapabilitySet = std::uint64_t;

namespace capability {

constexpr CapabilitySet retained_scene_transactions = 1ULL << 0U;
constexpr CapabilitySet renderer_scene_stream = 1ULL << 1U;
constexpr CapabilitySet image_resources = 1ULL << 2U;
constexpr CapabilitySet controller_policy = 1ULL << 3U;

} // namespace capability

struct Hello {
    ConnectionRole role;
    ProtocolVersion minimum_version;
    ProtocolVersion maximum_version;
    CapabilitySet required_capabilities;
    CapabilitySet optional_capabilities;
    std::string client_version;
};

struct ServerHandshake {
    ProtocolVersion minimum_version;
    ProtocolVersion maximum_version;
    CapabilitySet supported_capabilities;
    std::string server_version;
};

struct HelloAccepted {
    ProtocolVersion selected_version;
    CapabilitySet enabled_capabilities;
    std::string server_version;
};

enum class HandshakeError {
    invalid_version_range,
    unsupported_version,
    unsupported_required_capability,
};

using HandshakeResult = std::variant<HelloAccepted, HandshakeError>;

enum class HelloDecodeError {
    malformed_payload,
    invalid_role,
    invalid_version_range,
    invalid_client_version,
};

using HelloDecodeResult = std::variant<Hello, HelloDecodeError>;

enum class HelloAcceptedDecodeError {
    malformed_payload,
    invalid_server_version,
};

using HelloAcceptedDecodeResult = std::variant<HelloAccepted, HelloAcceptedDecodeError>;

std::vector<std::uint8_t> encode_hello(const Hello& hello);
HelloDecodeResult decode_hello(ByteView payload);
std::vector<std::uint8_t> encode_hello_accepted(const HelloAccepted& accepted);
HelloAcceptedDecodeResult decode_hello_accepted(ByteView payload);
HandshakeResult negotiate_hello(const Hello& hello, const ServerHandshake& server);

} // namespace mango_overlay::protocol
