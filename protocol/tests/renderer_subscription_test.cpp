#include "mango_overlay/protocol/renderer.hpp"

#include <cstdio>
#include <variant>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::RendererSubscription;
using mango_overlay::protocol::decode_renderer_subscription;
using mango_overlay::protocol::encode_renderer_subscription;

int main()
{
    const auto encoded = encode_renderer_subscription(RendererSubscription { 42 });
    const auto decoded = decode_renderer_subscription(
        ByteView { encoded.data(), encoded.size() });
    const auto* subscription = std::get_if<RendererSubscription>(&decoded);
    if (subscription == nullptr || subscription->known_revision != 42) {
        std::fputs("renderer subscription did not round-trip\n", stderr);
        return 1;
    }

    const auto malformed = decode_renderer_subscription(ByteView { nullptr, 0 });
    if (std::holds_alternative<RendererSubscription>(malformed)) {
        std::fputs("empty renderer subscription was accepted\n", stderr);
        return 1;
    }
    return 0;
}
