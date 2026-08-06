#include "mango_overlay/protocol/handshake.hpp"

#include <cstdio>
#include <variant>

using mango_overlay::protocol::ConnectionRole;
using mango_overlay::protocol::Hello;
using mango_overlay::protocol::HelloAccepted;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::protocol::ServerHandshake;
using mango_overlay::protocol::negotiate_hello;

int main()
{
    const Hello hello {
        ConnectionRole::provider,
        ProtocolVersion { 1, 0 },
        ProtocolVersion { 1, 4 },
        0x01,
        0x0e,
        "example.provider/1.0",
    };
    const ServerHandshake server {
        ProtocolVersion { 1, 1 },
        ProtocolVersion { 1, 2 },
        0x0b,
        "mango-overlayd/0.1",
    };

    const auto result = negotiate_hello(hello, server);
    const auto* accepted = std::get_if<HelloAccepted>(&result);
    if (accepted == nullptr) {
        std::fputs("compatible protocol ranges were rejected\n", stderr);
        return 1;
    }

    const bool matches = accepted->selected_version.major == 1
        && accepted->selected_version.minor == 2
        && accepted->enabled_capabilities == 0x0b
        && accepted->server_version == "mango-overlayd/0.1";
    if (!matches) {
        std::fputs("handshake did not select the highest common version and capability set\n", stderr);
        return 1;
    }

    return 0;
}
