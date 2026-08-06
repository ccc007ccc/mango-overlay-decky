#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/verifier.h>

#include <cstdio>
#include <variant>

using mango_overlay::protocol::HelloAccepted;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::decode_hello_accepted;
using mango_overlay::protocol::encode_hello_accepted;

int main()
{
    const auto encoded = encode_hello_accepted(HelloAccepted {
        ProtocolVersion { 1, 2 },
        0x2a,
        "mango-overlayd/0.1",
    });

    flatbuffers::Verifier verifier(encoded.data(), encoded.size());
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::HelloAccepted>(nullptr)) {
        std::fputs("encoded HelloAccepted is not a valid FlatBuffer\n", stderr);
        return 1;
    }

    const auto* accepted = flatbuffers::GetRoot<MangoOverlay::Wire::HelloAccepted>(encoded.data());
    const bool matches = accepted->selected_version()->major() == 1
        && accepted->selected_version()->minor() == 2
        && accepted->enabled_capabilities() == 0x2a
        && accepted->server_version()->str() == "mango-overlayd/0.1";
    if (!matches) {
        std::fputs("encoded HelloAccepted fields are incorrect\n", stderr);
        return 1;
    }


    const auto decoded = decode_hello_accepted(ByteView { encoded.data(), encoded.size() });
    const auto* decoded_accepted = std::get_if<HelloAccepted>(&decoded);
    if (decoded_accepted == nullptr
        || decoded_accepted->selected_version.major != 1
        || decoded_accepted->selected_version.minor != 2
        || decoded_accepted->enabled_capabilities != 0x2a
        || decoded_accepted->server_version != "mango-overlayd/0.1") {
        std::fputs("HelloAccepted did not round-trip\n", stderr);
        return 1;
    }

    return 0;
}
