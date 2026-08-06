#include "mango_overlay/broker/handshake_processor.hpp"
#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/verifier.h>

#include <cstdint>
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
        ProtocolVersion { 1, 2 },
        0x01,
        0x06,
        "example.provider/1.0",
    });
    const auto request = encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, 42 },
        ByteView { hello_payload.data(), hello_payload.size() });

    const auto response = process_initial_packet(
        ByteView { request.data(), request.size() },
        ServerHandshake {
            ProtocolVersion { 1, 0 },
            ProtocolVersion { 1, 1 },
            0x03,
            "mango-overlayd/0.1",
        });

    if (!response.accepted.has_value()
        || response.accepted->role != ConnectionRole::provider
        || response.accepted->version.major != 1
        || response.accepted->version.minor != 1
        || response.accepted->capabilities != 0x03) {
        std::fputs("broker discarded the negotiated session parameters\n", stderr);
        return 1;
    }

    const auto decoded_response = decode_packet(
        ByteView { response.response.data(), response.response.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded_response);
    if (packet == nullptr) {
        std::fputs("broker did not return a valid packet\n", stderr);
        return 1;
    }
    if (packet->header.message_type != MessageType::hello_accepted
        || packet->header.request_id != 42
        || packet->header.version.major != 1
        || packet->header.version.minor != 1) {
        std::fputs("broker returned an incorrect handshake packet header\n", stderr);
        return 1;
    }

    flatbuffers::Verifier verifier(packet->payload.data, packet->payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::HelloAccepted>(nullptr)) {
        std::fputs("broker returned an invalid HelloAccepted payload\n", stderr);
        return 1;
    }
    const auto* accepted = flatbuffers::GetRoot<MangoOverlay::Wire::HelloAccepted>(
        packet->payload.data);
    const bool matches = accepted->selected_version()->major() == 1
        && accepted->selected_version()->minor() == 1
        && accepted->enabled_capabilities() == 0x03
        && accepted->server_version()->str() == "mango-overlayd/0.1";
    if (!matches) {
        std::fputs("broker returned incorrect handshake values\n", stderr);
        return 1;
    }

    return 0;
}
