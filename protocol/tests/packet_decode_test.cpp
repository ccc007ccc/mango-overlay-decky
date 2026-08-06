#include "mango_overlay/protocol/packet.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <variant>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::decode_packet;

int main()
{
    constexpr std::array<std::uint8_t, 28> packet {
        'M', 'O', 'V', 'R',
        0x01, 0x00,
        0x00, 0x00,
        0x01, 0x00,
        0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xde, 0xad, 0xbe, 0xef,
    };

    const auto result = decode_packet(ByteView { packet.data(), packet.size() });
    const auto* decoded = std::get_if<DecodedPacketView>(&result);
    if (decoded == nullptr) {
        std::fputs("a valid packet was rejected\n", stderr);
        return 1;
    }

    const bool header_matches = decoded->header.version.major == 1
        && decoded->header.version.minor == 0
        && decoded->header.message_type == MessageType::hello
        && decoded->header.flags == 0
        && decoded->header.request_id == 0x0102030405060708ULL;
    if (!header_matches) {
        std::fputs("decoded packet header does not match the golden bytes\n", stderr);
        return 1;
    }

    constexpr std::array<std::uint8_t, 4> expected_payload { 0xde, 0xad, 0xbe, 0xef };
    if (decoded->payload.size != expected_payload.size()) {
        std::fputs("decoded payload size is incorrect\n", stderr);
        return 1;
    }
    for (std::size_t index = 0; index < expected_payload.size(); ++index) {
        if (decoded->payload.data[index] != expected_payload[index]) {
            std::fputs("decoded payload does not match the golden bytes\n", stderr);
            return 1;
        }
    }

    return 0;
}
