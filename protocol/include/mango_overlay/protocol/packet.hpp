#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace mango_overlay::protocol {

constexpr std::size_t packet_header_size = 24;
constexpr std::size_t maximum_payload_size = 1024 * 1024;
constexpr std::uint16_t packet_flag_file_descriptor = 1U << 0U;

struct ByteView {
    const std::uint8_t* data;
    std::size_t size;
};

struct ProtocolVersion {
    std::uint16_t major;
    std::uint16_t minor;
};

enum class MessageType : std::uint16_t {
    hello = 1,
    hello_accepted = 2,
    error = 3,
    register_provider = 4,
    provider_registered = 5,
    scene_transaction = 6,
    transaction_committed = 7,
    renderer_subscribe = 8,
    scene_snapshot_begin = 9,
    scene_snapshot_provider = 10,
    scene_snapshot_end = 11,
    provider_scene_updated = 12,
    provider_scene_removed = 13,
    upload_resource = 14,
    resource_stored = 15,
    release_resource = 16,
    resource_released = 17,
    resource_available = 18,
    controller_get_status = 19,
    controller_set_enabled = 20,
    controller_set_require_approval = 21,
    controller_set_application_policy = 22,
    controller_status = 23,
    controller_set_application_position = 24,
};

struct PacketHeader {
    ProtocolVersion version;
    MessageType message_type;
    std::uint16_t flags;
    std::uint64_t request_id;
};

struct DecodedPacketView {
    PacketHeader header;
    ByteView payload;
};

enum class DecodeError {
    truncated_header,
    invalid_magic,
    payload_too_large,
    size_mismatch,
};

using DecodeResult = std::variant<DecodedPacketView, DecodeError>;

std::vector<std::uint8_t> encode_packet(const PacketHeader& header, ByteView payload);
DecodeResult decode_packet(ByteView packet);

} // namespace mango_overlay::protocol
