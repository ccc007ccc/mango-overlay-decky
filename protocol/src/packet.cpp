#include "mango_overlay/protocol/packet.hpp"

#include <array>
#include <stdexcept>

namespace mango_overlay::protocol {

namespace {

constexpr std::array<std::uint8_t, 4> packet_magic { 'M', 'O', 'V', 'R' };

void append_u16(std::vector<std::uint8_t>& packet, std::uint16_t value)
{
    packet.push_back(static_cast<std::uint8_t>(value));
    packet.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& packet, std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        packet.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& packet, std::uint64_t value)
{
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        packet.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t read_u16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | static_cast<std::uint16_t>(bytes[1]) << 8U;
}

std::uint32_t read_u32(const std::uint8_t* bytes)
{
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[shift / 8]) << shift;
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* bytes)
{
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(bytes[shift / 8]) << shift;
    }
    return value;
}

} // namespace

std::vector<std::uint8_t> encode_packet(const PacketHeader& header, ByteView payload)
{
    if (payload.size > maximum_payload_size) {
        throw std::length_error("Mango Overlay packet payload exceeds the protocol limit");
    }
    if (payload.size != 0 && payload.data == nullptr) {
        throw std::invalid_argument("Mango Overlay packet payload is null");
    }

    std::vector<std::uint8_t> packet;
    packet.reserve(packet_header_size + payload.size);
    packet.insert(packet.end(), packet_magic.begin(), packet_magic.end());
    append_u16(packet, header.version.major);
    append_u16(packet, header.version.minor);
    append_u16(packet, static_cast<std::uint16_t>(header.message_type));
    append_u16(packet, header.flags);
    append_u32(packet, static_cast<std::uint32_t>(payload.size));
    append_u64(packet, header.request_id);
    if (payload.size != 0) {
        packet.insert(packet.end(), payload.data, payload.data + payload.size);
    }
    return packet;
}

DecodeResult decode_packet(ByteView packet)
{
    if (packet.size < packet_header_size || packet.data == nullptr) {
        return DecodeError::truncated_header;
    }
    for (std::size_t index = 0; index < packet_magic.size(); ++index) {
        if (packet.data[index] != packet_magic[index]) {
            return DecodeError::invalid_magic;
        }
    }

    const auto payload_size = read_u32(packet.data + 12);
    if (payload_size > maximum_payload_size) {
        return DecodeError::payload_too_large;
    }
    if (packet.size != packet_header_size + payload_size) {
        return DecodeError::size_mismatch;
    }

    return DecodedPacketView {
        PacketHeader {
            ProtocolVersion { read_u16(packet.data + 4), read_u16(packet.data + 6) },
            static_cast<MessageType>(read_u16(packet.data + 8)),
            read_u16(packet.data + 10),
            read_u64(packet.data + 16),
        },
        ByteView { packet.data + packet_header_size, payload_size },
    };
}

} // namespace mango_overlay::protocol
