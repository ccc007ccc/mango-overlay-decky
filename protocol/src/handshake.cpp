#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>

#include <algorithm>
#include <stdexcept>

namespace mango_overlay::protocol {

namespace {

bool version_less(const ProtocolVersion& left, const ProtocolVersion& right)
{
    return left.major < right.major
        || (left.major == right.major && left.minor < right.minor);
}

ProtocolVersion earlier_version(const ProtocolVersion& left, const ProtocolVersion& right)
{
    return version_less(left, right) ? left : right;
}

ProtocolVersion later_version(const ProtocolVersion& left, const ProtocolVersion& right)
{
    return version_less(left, right) ? right : left;
}

std::variant<ConnectionRole, HelloDecodeError> decode_role(MangoOverlay::Wire::Role role)
{
    switch (role) {
    case MangoOverlay::Wire::Role::Provider:
        return ConnectionRole::provider;
    case MangoOverlay::Wire::Role::Renderer:
        return ConnectionRole::renderer;
    case MangoOverlay::Wire::Role::Controller:
        return ConnectionRole::controller;
    case MangoOverlay::Wire::Role::Unknown:
        return HelloDecodeError::invalid_role;
    }
    return HelloDecodeError::invalid_role;
}

MangoOverlay::Wire::Role encode_role(ConnectionRole role)
{
    switch (role) {
    case ConnectionRole::provider:
        return MangoOverlay::Wire::Role::Provider;
    case ConnectionRole::renderer:
        return MangoOverlay::Wire::Role::Renderer;
    case ConnectionRole::controller:
        return MangoOverlay::Wire::Role::Controller;
    }
    throw std::invalid_argument("Mango Overlay Hello has an invalid connection role");
}

bool valid_implementation_version(const std::string& version)
{
    return !version.empty() && version.size() <= 128
        && std::find(version.begin(), version.end(), '\0') == version.end();
}

} // namespace

std::vector<std::uint8_t> encode_hello(const Hello& hello)
{
    if (version_less(hello.maximum_version, hello.minimum_version)) {
        throw std::invalid_argument("Mango Overlay Hello has an invalid protocol version range");
    }
    if (!valid_implementation_version(hello.client_version)) {
        throw std::invalid_argument("Mango Overlay Hello has an invalid client version");
    }

    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Version minimum_version(
        hello.minimum_version.major, hello.minimum_version.minor);
    const MangoOverlay::Wire::Version maximum_version(
        hello.maximum_version.major, hello.maximum_version.minor);
    const auto client_version = builder.CreateString(hello.client_version);
    const auto wire_hello = MangoOverlay::Wire::CreateHello(
        builder,
        encode_role(hello.role),
        &minimum_version,
        &maximum_version,
        hello.required_capabilities,
        hello.optional_capabilities,
        client_version);
    builder.Finish(wire_hello);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

HelloDecodeResult decode_hello(ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return HelloDecodeError::malformed_payload;
    }

    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::Hello>(nullptr)) {
        return HelloDecodeError::malformed_payload;
    }

    const auto* wire_hello = flatbuffers::GetRoot<MangoOverlay::Wire::Hello>(payload.data);
    const auto* wire_minimum = wire_hello->minimum_version();
    const auto* wire_maximum = wire_hello->maximum_version();
    const auto* wire_client_version = wire_hello->client_version();
    if (wire_minimum == nullptr || wire_maximum == nullptr || wire_client_version == nullptr) {
        return HelloDecodeError::malformed_payload;
    }

    const auto decoded_role = decode_role(wire_hello->role());
    const auto* role = std::get_if<ConnectionRole>(&decoded_role);
    if (role == nullptr) {
        return std::get<HelloDecodeError>(decoded_role);
    }

    const ProtocolVersion minimum { wire_minimum->major(), wire_minimum->minor() };
    const ProtocolVersion maximum { wire_maximum->major(), wire_maximum->minor() };
    if (version_less(maximum, minimum)) {
        return HelloDecodeError::invalid_version_range;
    }

    const std::string client_version = wire_client_version->str();
    if (!valid_implementation_version(client_version)) {
        return HelloDecodeError::invalid_client_version;
    }

    return Hello {
        *role,
        minimum,
        maximum,
        wire_hello->required_capabilities(),
        wire_hello->optional_capabilities(),
        client_version,
    };
}

std::vector<std::uint8_t> encode_hello_accepted(const HelloAccepted& accepted)
{
    if (!valid_implementation_version(accepted.server_version)) {
        throw std::invalid_argument("Mango Overlay HelloAccepted has an invalid server version");
    }

    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Version selected_version(
        accepted.selected_version.major, accepted.selected_version.minor);
    const auto server_version = builder.CreateString(accepted.server_version);
    const auto wire_accepted = MangoOverlay::Wire::CreateHelloAccepted(
        builder,
        &selected_version,
        accepted.enabled_capabilities,
        server_version);
    builder.Finish(wire_accepted);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

HelloAcceptedDecodeResult decode_hello_accepted(ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return HelloAcceptedDecodeError::malformed_payload;
    }
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::HelloAccepted>(nullptr)) {
        return HelloAcceptedDecodeError::malformed_payload;
    }

    const auto* wire = flatbuffers::GetRoot<MangoOverlay::Wire::HelloAccepted>(payload.data);
    if (wire->selected_version() == nullptr || wire->server_version() == nullptr) {
        return HelloAcceptedDecodeError::malformed_payload;
    }
    const std::string server_version = wire->server_version()->str();
    if (!valid_implementation_version(server_version)) {
        return HelloAcceptedDecodeError::invalid_server_version;
    }
    return HelloAccepted {
        ProtocolVersion {
            wire->selected_version()->major(), wire->selected_version()->minor() },
        wire->enabled_capabilities(),
        server_version,
    };
}

HandshakeResult negotiate_hello(const Hello& hello, const ServerHandshake& server)
{
    if (version_less(hello.maximum_version, hello.minimum_version)
        || version_less(server.maximum_version, server.minimum_version)) {
        return HandshakeError::invalid_version_range;
    }

    const auto common_minimum = later_version(hello.minimum_version, server.minimum_version);
    const auto common_maximum = earlier_version(hello.maximum_version, server.maximum_version);
    if (version_less(common_maximum, common_minimum)) {
        return HandshakeError::unsupported_version;
    }
    if ((hello.required_capabilities & ~server.supported_capabilities) != 0) {
        return HandshakeError::unsupported_required_capability;
    }

    return HelloAccepted {
        common_maximum,
        hello.required_capabilities | (hello.optional_capabilities & server.supported_capabilities),
        server.server_version,
    };
}

} // namespace mango_overlay::protocol
