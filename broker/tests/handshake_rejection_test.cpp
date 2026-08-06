#include "mango_overlay/broker/handshake_processor.hpp"
#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/verifier.h>

#include <cstdio>
#include <variant>

using mango_overlay::broker::process_initial_packet;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ConnectionRole;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::Hello;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::ServerHandshake;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_hello;
using mango_overlay::protocol::encode_packet;

int main()
{
    const auto hello_payload = encode_hello(Hello {
        ConnectionRole::provider,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 0 },
        0x08,
        0,
        "unsupported.provider/1.0",
    });
    const auto request = encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, 43 },
        ByteView { hello_payload.data(), hello_payload.size() });

    const auto response = process_initial_packet(
        ByteView { request.data(), request.size() },
        ServerHandshake {
            ProtocolVersion { 1, 0 },
            ProtocolVersion { 1, 0 },
            0x03,
            "mango-overlayd/0.1",
        });
    if (response.accepted.has_value()) {
        std::fputs("broker accepted an unsupported required capability\n", stderr);
        return 1;
    }
    const auto decoded_response = decode_packet(
        ByteView { response.response.data(), response.response.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded_response);
    if (packet == nullptr || packet->header.message_type != MessageType::error
        || packet->header.request_id != 43) {
        std::fputs("broker did not return a correlated error packet\n", stderr);
        return 1;
    }

    flatbuffers::Verifier verifier(packet->payload.data, packet->payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ErrorResponse>(nullptr)) {
        std::fputs("broker returned an invalid error payload\n", stderr);
        return 1;
    }
    const auto* error = flatbuffers::GetRoot<MangoOverlay::Wire::ErrorResponse>(
        packet->payload.data);
    if (error->code() != MangoOverlay::Wire::ErrorCode::UnsupportedRequiredCapability) {
        std::fputs("broker returned the wrong handshake error code\n", stderr);
        return 1;
    }

    return 0;
}
