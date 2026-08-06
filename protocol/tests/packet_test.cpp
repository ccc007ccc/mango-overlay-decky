#include "mango_overlay/protocol/packet.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::encode_packet;

int main()
{
    constexpr std::array<std::uint8_t, 4> payload { 0xde, 0xad, 0xbe, 0xef };
    constexpr std::array<std::uint8_t, 28> expected {
        'M', 'O', 'V', 'R',
        0x01, 0x00,
        0x00, 0x00,
        0x01, 0x00,
        0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xde, 0xad, 0xbe, 0xef,
    };

    const auto encoded = encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::hello, 0, 0x0102030405060708ULL },
        ByteView { payload.data(), payload.size() });

    if (encoded != std::vector<std::uint8_t>(expected.begin(), expected.end())) {
        std::fputs("packet encoding does not match the protocol's golden bytes\n", stderr);
        return 1;
    }

    return 0;
}
