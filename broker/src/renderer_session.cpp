#include "mango_overlay/broker/renderer_session.hpp"
#include "mango_overlay/broker/snapshot_encoder.hpp"
#include "mango_overlay/protocol/renderer.hpp"

#include <algorithm>
#include <iterator>
#include <variant>

namespace mango_overlay::broker {

RendererSession::RendererSession(
    scene::SceneStore& scenes,
    protocol::ProtocolVersion version)
    : scenes_(scenes)
    , version_(version)
{
}

RendererSessionResponse RendererSession::reject(
    std::uint64_t request_id,
    protocol::ErrorCode code,
    const char* message)
{
    constexpr std::uint8_t maximum_protocol_errors = 3;
    const auto payload = protocol::encode_error(protocol::ProtocolError { code, message });
    ++protocol_errors_;
    return {
        { OutboundPacket { protocol::encode_packet(
            protocol::PacketHeader {
                version_,
                protocol::MessageType::error,
                0,
                request_id,
            },
            protocol::ByteView { payload.data(), payload.size() }), nullptr } },
        protocol_errors_ >= maximum_protocol_errors,
    };
}

RendererSessionResponse RendererSession::process(protocol::ByteView packet_bytes)
{
    const auto decoded = protocol::decode_packet(packet_bytes);
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded);
    if (packet == nullptr) {
        return reject(
            0,
            protocol::ErrorCode::malformed_packet,
            "The packet header or size is invalid");
    }
    if (packet->header.version.major != version_.major
        || packet->header.version.minor != version_.minor) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::unsupported_version,
            "The packet version differs from the negotiated version");
    }
    if (packet->header.flags != 0) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::malformed_packet,
            "The packet contains unsupported flags");
    }
    if (packet->header.message_type != protocol::MessageType::renderer_subscribe) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::unexpected_message,
            "A renderer may only request a complete scene snapshot");
    }

    const auto decoded_subscription = protocol::decode_renderer_subscription(packet->payload);
    if (!std::holds_alternative<protocol::RendererSubscription>(decoded_subscription)) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::malformed_payload,
            "The renderer subscription is invalid");
    }

    const auto snapshot = scenes_.snapshot();
    auto packets = encode_snapshot_packets(
        *snapshot, version_, packet->header.request_id);
    revision_ = snapshot->revision;
    subscribed_ = true;
    return { std::move(packets), false };
}

RendererSessionResponse RendererSession::wait_for_updates(std::chrono::milliseconds timeout)
{
    if (!subscribed_) {
        return { {}, true };
    }

    const auto changes = scenes_.wait_for_changes_after(revision_, timeout);
    if (changes.history_gap) {
        const auto snapshot = scenes_.snapshot();
        auto packets = encode_snapshot_packets(*snapshot, version_, 0);
        revision_ = snapshot->revision;
        return { std::move(packets), false };
    }

    if (std::any_of(
            changes.changes.begin(),
            changes.changes.end(),
            [](const scene::SceneChange& change) {
                return change.kind == scene::SceneChangeKind::reset;
            })) {
        const auto snapshot = scenes_.snapshot();
        auto packets = encode_snapshot_packets(*snapshot, version_, 0);
        revision_ = snapshot->revision;
        return { std::move(packets), false };
    }

    std::vector<OutboundPacket> packets;
    for (const auto& change : changes.changes) {
        auto encoded = encode_scene_change_packets(change, version_);
        packets.insert(
            packets.end(),
            std::make_move_iterator(encoded.begin()),
            std::make_move_iterator(encoded.end()));
        revision_ = change.revision;
    }
    return { std::move(packets), false };
}

} // namespace mango_overlay::broker
