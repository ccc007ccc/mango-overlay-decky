#include "mango_overlay/broker/handshake_processor.hpp"
#include "mango_overlay/protocol/error.hpp"

#include <variant>

namespace mango_overlay::broker {

namespace {

std::vector<std::uint8_t> make_error_packet(
    std::uint64_t request_id,
    protocol::ErrorCode code,
    const char* message)
{
    const auto payload = protocol::encode_error(protocol::ProtocolError { code, message });
    return protocol::encode_packet(
        protocol::PacketHeader {
            protocol::ProtocolVersion { 1, 0 },
            protocol::MessageType::error,
            0,
            request_id,
        },
        protocol::ByteView { payload.data(), payload.size() });
}

} // namespace

InitialPacketResult process_initial_packet(
    protocol::ByteView packet_bytes,
    const protocol::ServerHandshake& server)
{
    const auto decoded_packet = protocol::decode_packet(packet_bytes);
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded_packet);
    if (packet == nullptr
        || packet->header.message_type != protocol::MessageType::hello
        || packet->header.version.major != 1
        || packet->header.version.minor != 0
        || packet->header.flags != 0) {
        return {};
    }

    const auto decoded_hello = protocol::decode_hello(packet->payload);
    const auto* hello = std::get_if<protocol::Hello>(&decoded_hello);
    if (hello == nullptr) {
        return {};
    }

    const auto negotiation = protocol::negotiate_hello(*hello, server);
    const auto* accepted = std::get_if<protocol::HelloAccepted>(&negotiation);
    if (accepted == nullptr) {
        switch (std::get<protocol::HandshakeError>(negotiation)) {
        case protocol::HandshakeError::invalid_version_range:
        case protocol::HandshakeError::unsupported_version:
            return {
                make_error_packet(
                    packet->header.request_id,
                    protocol::ErrorCode::unsupported_version,
                    "No supported protocol version is shared"),
                std::nullopt,
            };
        case protocol::HandshakeError::unsupported_required_capability:
            return {
                make_error_packet(
                    packet->header.request_id,
                    protocol::ErrorCode::unsupported_required_capability,
                    "A required protocol capability is unavailable"),
                std::nullopt,
            };
        }
    }

    const auto response_payload = protocol::encode_hello_accepted(*accepted);
    return {
        protocol::encode_packet(
            protocol::PacketHeader {
                accepted->selected_version,
                protocol::MessageType::hello_accepted,
                0,
                packet->header.request_id,
            },
            protocol::ByteView { response_payload.data(), response_payload.size() }),
        AcceptedSession {
            hello->role,
            accepted->selected_version,
            accepted->enabled_capabilities,
        },
    };
}

} // namespace mango_overlay::broker
