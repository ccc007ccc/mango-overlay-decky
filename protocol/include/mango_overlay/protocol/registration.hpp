#pragma once

#include "mango_overlay/protocol/packet.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace mango_overlay::protocol {

enum class Visibility : std::uint8_t {
    game_only = 0,
    steam_only = 1,
    always = 2,
};

struct ProviderRegistration {
    std::string application_id;
    std::string instance_id;
    std::string display_name;
    std::uint16_t canvas_width;
    std::uint16_t canvas_height;
    Visibility requested_visibility;
};

enum class RegistrationDecodeError {
    malformed_payload,
    invalid_identity,
    invalid_canvas,
    invalid_visibility,
};

using RegistrationDecodeResult = std::variant<ProviderRegistration, RegistrationDecodeError>;

RegistrationDecodeResult decode_registration(ByteView payload);
std::vector<std::uint8_t> encode_provider_registered(std::uint64_t scene_revision);

} // namespace mango_overlay::protocol
