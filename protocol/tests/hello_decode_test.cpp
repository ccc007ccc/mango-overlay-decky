#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>

#include <cstdio>
#include <variant>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ConnectionRole;
using mango_overlay::protocol::Hello;
using mango_overlay::protocol::decode_hello;

int main()
{
    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Version minimum_version(1, 0);
    const MangoOverlay::Wire::Version maximum_version(1, 2);
    const auto client_version = builder.CreateString("example.provider/2.4.1");
    const auto wire_hello = MangoOverlay::Wire::CreateHello(
        builder,
        MangoOverlay::Wire::Role::Provider,
        &minimum_version,
        &maximum_version,
        0x05,
        0x0a,
        client_version);
    builder.Finish(wire_hello);

    const auto result = decode_hello(ByteView { builder.GetBufferPointer(), builder.GetSize() });
    const auto* hello = std::get_if<Hello>(&result);
    if (hello == nullptr) {
        std::fputs("a valid provider Hello was rejected\n", stderr);
        return 1;
    }

    const bool matches = hello->role == ConnectionRole::provider
        && hello->minimum_version.major == 1
        && hello->minimum_version.minor == 0
        && hello->maximum_version.major == 1
        && hello->maximum_version.minor == 2
        && hello->required_capabilities == 0x05
        && hello->optional_capabilities == 0x0a
        && hello->client_version == "example.provider/2.4.1";
    if (!matches) {
        std::fputs("decoded Hello fields do not match the wire payload\n", stderr);
        return 1;
    }

    return 0;
}
