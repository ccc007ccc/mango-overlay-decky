#include "mango_overlay/protocol/registration.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>

#include <algorithm>

namespace mango_overlay::protocol {

namespace {

bool valid_string(const std::string& value)
{
    return !value.empty() && value.size() <= 128
        && std::find(value.begin(), value.end(), '\0') == value.end();
}

std::variant<Visibility, RegistrationDecodeError> decode_visibility(
    MangoOverlay::Wire::Visibility visibility)
{
    switch (visibility) {
    case MangoOverlay::Wire::Visibility::GameOnly:
        return Visibility::game_only;
    case MangoOverlay::Wire::Visibility::SteamOnly:
        return Visibility::steam_only;
    case MangoOverlay::Wire::Visibility::Always:
        return Visibility::always;
    }
    return RegistrationDecodeError::invalid_visibility;
}

} // namespace

RegistrationDecodeResult decode_registration(ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return RegistrationDecodeError::malformed_payload;
    }
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::RegisterProvider>(nullptr)) {
        return RegistrationDecodeError::malformed_payload;
    }

    const auto* wire = flatbuffers::GetRoot<MangoOverlay::Wire::RegisterProvider>(payload.data);
    if (wire->application_id() == nullptr || wire->instance_id() == nullptr
        || wire->display_name() == nullptr) {
        return RegistrationDecodeError::malformed_payload;
    }

    const std::string application_id = wire->application_id()->str();
    const std::string instance_id = wire->instance_id()->str();
    const std::string display_name = wire->display_name()->str();
    if (!valid_string(application_id) || !valid_string(instance_id)
        || !valid_string(display_name)) {
        return RegistrationDecodeError::invalid_identity;
    }
    if (wire->canvas_width() == 0 || wire->canvas_width() > 8192
        || wire->canvas_height() == 0 || wire->canvas_height() > 8192) {
        return RegistrationDecodeError::invalid_canvas;
    }

    const auto visibility_result = decode_visibility(wire->requested_visibility());
    const auto* visibility = std::get_if<Visibility>(&visibility_result);
    if (visibility == nullptr) {
        return std::get<RegistrationDecodeError>(visibility_result);
    }

    return ProviderRegistration {
        application_id,
        instance_id,
        display_name,
        wire->canvas_width(),
        wire->canvas_height(),
        *visibility,
    };
}

std::vector<std::uint8_t> encode_provider_registered(std::uint64_t scene_revision)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto registered = MangoOverlay::Wire::CreateProviderRegistered(builder, scene_revision);
    builder.Finish(registered);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

} // namespace mango_overlay::protocol
