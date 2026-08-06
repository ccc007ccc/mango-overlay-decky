#pragma once

#include "mango_overlay/scene/store.hpp"

#include <cstdint>
#include <optional>

namespace mango_overlay::renderer {

struct CanvasTransform {
    float scale;
    scene::Vec2 viewport_size;

    scene::Vec2 point(scene::Vec2 value) const;
    scene::Vec2 size(scene::Vec2 value) const;
};

std::optional<CanvasTransform> fit_canvas(
    std::uint16_t canvas_width,
    std::uint16_t canvas_height,
    float output_width,
    float output_height);

bool visible_for_focus(scene::Visibility visibility, bool steam_focused);
bool snapshot_has_visible_provider(
    const scene::SceneSnapshot& snapshot,
    bool steam_focused);

} // namespace mango_overlay::renderer
