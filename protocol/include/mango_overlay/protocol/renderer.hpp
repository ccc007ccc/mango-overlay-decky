#pragma once

#include "mango_overlay/protocol/packet.hpp"

#include <cstdint>
#include <variant>
#include <vector>

namespace mango_overlay::protocol {

struct RendererSubscription {
    std::uint64_t known_revision;
};

enum class RendererSubscriptionDecodeError {
    malformed_payload,
};

using RendererSubscriptionDecodeResult
    = std::variant<RendererSubscription, RendererSubscriptionDecodeError>;

std::vector<std::uint8_t> encode_renderer_subscription(
    const RendererSubscription& subscription);
RendererSubscriptionDecodeResult decode_renderer_subscription(ByteView payload);

} // namespace mango_overlay::protocol
