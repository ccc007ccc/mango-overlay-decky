#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/verifier.h>

#include <cstdio>

using mango_overlay::protocol::ConnectionRole;
using mango_overlay::protocol::Hello;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::encode_hello;

int main()
{
    const Hello hello {
        ConnectionRole::renderer,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 3 },
        0x11,
        0x24,
        "example.renderer/3.0",
    };

    const auto encoded = encode_hello(hello);
    flatbuffers::Verifier verifier(encoded.data(), encoded.size());
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::Hello>(nullptr)) {
        std::fputs("encoded Hello is not a valid FlatBuffer\n", stderr);
        return 1;
    }

    const auto* wire_hello = flatbuffers::GetRoot<MangoOverlay::Wire::Hello>(encoded.data());
    const bool matches = wire_hello->role() == MangoOverlay::Wire::Role::Renderer
        && wire_hello->minimum_version()->major() == 1
        && wire_hello->minimum_version()->minor() == 0
        && wire_hello->maximum_version()->major() == 1
        && wire_hello->maximum_version()->minor() == 3
        && wire_hello->required_capabilities() == 0x11
        && wire_hello->optional_capabilities() == 0x24
        && wire_hello->client_version()->str() == "example.renderer/3.0";
    if (!matches) {
        std::fputs("encoded Hello fields are incorrect\n", stderr);
        return 1;
    }

    return 0;
}
