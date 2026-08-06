#include "mango_overlay/protocol/registration.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>

#include <cstdio>
#include <variant>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ProviderRegistration;
using mango_overlay::protocol::Visibility;
using mango_overlay::protocol::decode_registration;

int main()
{
    flatbuffers::FlatBufferBuilder builder;
    const auto application_id = builder.CreateString("example.telemetry");
    const auto instance_id = builder.CreateString("primary");
    const auto display_name = builder.CreateString("Example Telemetry");
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        application_id,
        instance_id,
        display_name,
        1920,
        1080,
        MangoOverlay::Wire::Visibility::Always);
    builder.Finish(registration);

    const auto result = decode_registration(
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
    const auto* decoded = std::get_if<ProviderRegistration>(&result);
    if (decoded == nullptr) {
        std::fputs("valid provider registration was rejected\n", stderr);
        return 1;
    }

    const bool matches = decoded->application_id == "example.telemetry"
        && decoded->instance_id == "primary"
        && decoded->display_name == "Example Telemetry"
        && decoded->canvas_width == 1920
        && decoded->canvas_height == 1080
        && decoded->requested_visibility == Visibility::always;
    if (!matches) {
        std::fputs("decoded provider registration fields are incorrect\n", stderr);
        return 1;
    }

    return 0;
}
