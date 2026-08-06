#include "mango_overlay/broker/provider_session.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <variant>
#include <vector>

using mango_overlay::broker::ProviderSession;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::decode_packet;
using mango_overlay::protocol::encode_packet;
using mango_overlay::scene::SceneStore;

namespace {

std::vector<std::uint8_t> unexpected_packet(std::uint64_t request_id)
{
    return encode_packet(
        PacketHeader {
            ProtocolVersion { 1, 0 },
            MessageType::scene_transaction,
            0,
            request_id,
        },
        ByteView { nullptr, 0 });
}

bool is_correlated_error(
    const std::vector<std::uint8_t>& response,
    std::uint64_t request_id,
    MangoOverlay::Wire::ErrorCode expected_code)
{
    const auto decoded = decode_packet(ByteView { response.data(), response.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    if (packet == nullptr || packet->header.message_type != MessageType::error
        || packet->header.request_id != request_id) {
        return false;
    }

    flatbuffers::Verifier verifier(packet->payload.data, packet->payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ErrorResponse>(nullptr)) {
        return false;
    }
    return flatbuffers::GetRoot<MangoOverlay::Wire::ErrorResponse>(packet->payload.data)
               ->code()
        == expected_code;
}

} // namespace

int main()
{
    SceneStore scenes;
    ProviderSession session(scenes, 300, ProtocolVersion { 1, 0 });

    for (std::uint64_t request_id = 31; request_id <= 33; ++request_id) {
        const auto request = unexpected_packet(request_id);
        const auto response = session.process(ByteView { request.data(), request.size() });
        if (!is_correlated_error(
                response.packet,
                request_id,
                MangoOverlay::Wire::ErrorCode::UnexpectedMessage)) {
            std::fputs("provider session did not return a correlated protocol error\n", stderr);
            return 1;
        }

        const bool should_close = request_id == 33;
        if (response.close_after_send != should_close) {
            std::fputs("provider session closed at the wrong error threshold\n", stderr);
            return 1;
        }
    }

    if (!scenes.snapshot()->providers.empty()) {
        std::fputs("rejected requests changed the published scene\n", stderr);
        return 1;
    }
    return 0;
}
