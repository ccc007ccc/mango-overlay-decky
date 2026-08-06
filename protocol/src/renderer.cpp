#include "mango_overlay/protocol/renderer.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

namespace mango_overlay::protocol {

std::vector<std::uint8_t> encode_renderer_subscription(
    const RendererSubscription& subscription)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto wire = MangoOverlay::Wire::CreateRendererSubscribe(
        builder, subscription.known_revision);
    builder.Finish(wire);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

RendererSubscriptionDecodeResult decode_renderer_subscription(ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return RendererSubscriptionDecodeError::malformed_payload;
    }
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::RendererSubscribe>(nullptr)) {
        return RendererSubscriptionDecodeError::malformed_payload;
    }
    const auto* wire = flatbuffers::GetRoot<MangoOverlay::Wire::RendererSubscribe>(
        payload.data);
    return RendererSubscription { wire->known_revision() };
}

} // namespace mango_overlay::protocol
