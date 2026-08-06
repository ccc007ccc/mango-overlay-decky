#include "mango_overlay/renderer/canvas.hpp"

#include <algorithm>
#include <cmath>

namespace mango_overlay::renderer {

scene::Vec2 CanvasTransform::point(scene::Vec2 value) const
{
    return { value.x * scale, value.y * scale };
}

scene::Vec2 CanvasTransform::size(scene::Vec2 value) const
{
    return { value.x * scale, value.y * scale };
}

std::optional<CanvasTransform> fit_canvas(
    std::uint16_t canvas_width,
    std::uint16_t canvas_height,
    float output_width,
    float output_height)
{
    if (canvas_width == 0 || canvas_height == 0
        || !std::isfinite(output_width) || output_width <= 0.0F
        || !std::isfinite(output_height) || output_height <= 0.0F) {
        return std::nullopt;
    }

    const float scale = std::min(
        output_width / static_cast<float>(canvas_width),
        output_height / static_cast<float>(canvas_height));
    return CanvasTransform {
        scale,
        scene::Vec2 {
            output_width / scale,
            output_height / scale,
        },
    };
}

bool visible_for_focus(scene::Visibility visibility, bool steam_focused)
{
    switch (visibility) {
    case scene::Visibility::game_only:
        return !steam_focused;
    case scene::Visibility::steam_only:
        return steam_focused;
    case scene::Visibility::always:
        return true;
    }
    return false;
}

bool snapshot_has_visible_provider(
    const scene::SceneSnapshot& snapshot,
    bool steam_focused)
{
    return std::any_of(
        snapshot.providers.begin(), snapshot.providers.end(), [&](const auto& provider) {
            return provider != nullptr
                && visible_for_focus(provider->identity.requested_visibility, steam_focused);
        });
}

} // namespace mango_overlay::renderer
