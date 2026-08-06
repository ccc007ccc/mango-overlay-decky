#pragma once

#include "mango_overlay/scene/store.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace mango_overlay::renderer {

struct AffineTransform {
    float xx = 1.0F;
    float xy = 0.0F;
    float yx = 0.0F;
    float yy = 1.0F;
    float x = 0.0F;
    float y = 0.0F;

    scene::Vec2 point(scene::Vec2 value) const;
};

struct ScreenRect {
    scene::Vec2 minimum;
    scene::Vec2 maximum;
};

struct ResolvedElement {
    const scene::Element* element = nullptr;
    AffineTransform transform;
    float opacity = 1.0F;
    std::optional<ScreenRect> clip;
    bool visible = true;
};

class LayoutResolver {
public:
    bool resolve(
        const scene::ProviderScene& provider,
        float output_width,
        float output_height,
        std::vector<ResolvedElement>& output);

private:
    bool resolve_element(
        const scene::ProviderScene& provider,
        std::size_t index,
        const AffineTransform& canvas,
        scene::Vec2 viewport_size,
        const ScreenRect& canvas_clip);

    std::unordered_map<scene::ElementId, std::size_t> indices_;
    std::vector<std::uint8_t> state_;
    std::vector<AffineTransform> transforms_;
    std::vector<float> opacities_;
    std::vector<std::optional<ScreenRect>> clips_;
    std::vector<bool> visibility_;
};

} // namespace mango_overlay::renderer
