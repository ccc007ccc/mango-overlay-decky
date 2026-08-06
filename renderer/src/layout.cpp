#include "mango_overlay/renderer/layout.hpp"

#include "mango_overlay/renderer/canvas.hpp"
#include <algorithm>
#include <cmath>
#include <variant>

namespace mango_overlay::renderer {

namespace {

constexpr float pi = 3.14159265358979323846F;

AffineTransform multiply(const AffineTransform& left, const AffineTransform& right)
{
    return {
        left.xx * right.xx + left.xy * right.yx,
        left.xx * right.xy + left.xy * right.yy,
        left.yx * right.xx + left.yy * right.yx,
        left.yx * right.xy + left.yy * right.yy,
        left.xx * right.x + left.xy * right.y + left.x,
        left.yx * right.x + left.yy * right.y + left.y,
    };
}

AffineTransform translation(scene::Vec2 offset)
{
    return { 1.0F, 0.0F, 0.0F, 1.0F, offset.x, offset.y };
}

AffineTransform element_transform(const scene::ElementTransform& transform)
{
    const float radians = transform.rotation_degrees * pi / 180.0F;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return {
        cosine * transform.scale.x,
        -sine * transform.scale.y,
        sine * transform.scale.x,
        cosine * transform.scale.y,
        transform.translation.x,
        transform.translation.y,
    };
}

scene::Vec2 anchor_offset(
    scene::Anchor anchor,
    scene::Vec2 viewport_size)
{
    const float width = viewport_size.x;
    const float height = viewport_size.y;
    switch (anchor) {
    case scene::Anchor::top_left:
        return { 0.0F, 0.0F };
    case scene::Anchor::top_center:
        return { width / 2.0F, 0.0F };
    case scene::Anchor::top_right:
        return { width, 0.0F };
    case scene::Anchor::center_left:
        return { 0.0F, height / 2.0F };
    case scene::Anchor::center:
        return { width / 2.0F, height / 2.0F };
    case scene::Anchor::center_right:
        return { width, height / 2.0F };
    case scene::Anchor::bottom_left:
        return { 0.0F, height };
    case scene::Anchor::bottom_center:
        return { width / 2.0F, height };
    case scene::Anchor::bottom_right:
        return { width, height };
    }
    return { 0.0F, 0.0F };
}

ScreenRect transformed_bounds(
    const AffineTransform& transform,
    const scene::ClipRect& rectangle)
{
    const scene::Vec2 bottom_right {
        rectangle.position.x + rectangle.size.x,
        rectangle.position.y + rectangle.size.y,
    };
    const scene::Vec2 corners[] = {
        transform.point(rectangle.position),
        transform.point({ bottom_right.x, rectangle.position.y }),
        transform.point(bottom_right),
        transform.point({ rectangle.position.x, bottom_right.y }),
    };
    ScreenRect result { corners[0], corners[0] };
    for (const auto point : corners) {
        result.minimum.x = std::min(result.minimum.x, point.x);
        result.minimum.y = std::min(result.minimum.y, point.y);
        result.maximum.x = std::max(result.maximum.x, point.x);
        result.maximum.y = std::max(result.maximum.y, point.y);
    }
    return result;
}

std::optional<ScreenRect> intersect(ScreenRect left, ScreenRect right)
{
    ScreenRect result {
        { std::max(left.minimum.x, right.minimum.x),
            std::max(left.minimum.y, right.minimum.y) },
        { std::min(left.maximum.x, right.maximum.x),
            std::min(left.maximum.y, right.maximum.y) },
    };
    if (result.minimum.x >= result.maximum.x
        || result.minimum.y >= result.maximum.y) {
        return std::nullopt;
    }
    return result;
}

} // namespace

scene::Vec2 AffineTransform::point(scene::Vec2 value) const
{
    return {
        xx * value.x + xy * value.y + x,
        yx * value.x + yy * value.y + y,
    };
}

bool LayoutResolver::resolve_element(
    const scene::ProviderScene& provider,
    std::size_t index,
    const AffineTransform& canvas,
    scene::Vec2 viewport_size,
    const ScreenRect& canvas_clip)
{
    if (state_[index] == 2) {
        return true;
    }
    if (state_[index] == 1) {
        return false;
    }
    state_[index] = 1;
    const auto& element = provider.elements[index];

    AffineTransform parent = canvas;
    float parent_opacity = 1.0F;
    bool parent_visible = true;
    std::optional<ScreenRect> parent_clip = canvas_clip;
    if (element.parent_id == 0) {
        parent = multiply(
            parent,
            translation(anchor_offset(
                element.transform.anchor,
                viewport_size)));
    } else {
        const auto parent_entry = indices_.find(element.parent_id);
        if (parent_entry == indices_.end()
            || !std::holds_alternative<scene::GroupElement>(
                provider.elements[parent_entry->second].content)
            || element.transform.anchor != scene::Anchor::top_left
            || !resolve_element(
                provider,
                parent_entry->second,
                canvas,
                viewport_size,
                canvas_clip)) {
            return false;
        }
        parent = transforms_[parent_entry->second];
        parent_opacity = opacities_[parent_entry->second];
        parent_visible = visibility_[parent_entry->second];
        parent_clip = clips_[parent_entry->second];
    }

    transforms_[index] = multiply(parent, element_transform(element.transform));
    opacities_[index] = parent_opacity * element.transform.opacity;
    visibility_[index] = parent_visible && !element.transform.hidden
        && opacities_[index] > 0.0F && parent_clip.has_value();
    clips_[index] = parent_clip;
    if (element.transform.clip.has_value() && clips_[index].has_value()) {
        clips_[index] = intersect(
            *clips_[index],
            transformed_bounds(transforms_[index], *element.transform.clip));
        visibility_[index] = visibility_[index] && clips_[index].has_value();
    }
    state_[index] = 2;
    return true;
}

bool LayoutResolver::resolve(
    const scene::ProviderScene& provider,
    float output_width,
    float output_height,
    std::vector<ResolvedElement>& output)
{
    output.clear();
    const auto fit = fit_canvas(
        provider.identity.canvas_width,
        provider.identity.canvas_height,
        output_width,
        output_height);
    if (!fit.has_value()) {
        return false;
    }

    const AffineTransform canvas {
        fit->scale, 0.0F, 0.0F, fit->scale, 0.0F, 0.0F };
    const ScreenRect canvas_clip {
        { 0.0F, 0.0F },
        { output_width, output_height },
    };

    indices_.clear();
    indices_.reserve(provider.elements.size());
    for (std::size_t index = 0; index < provider.elements.size(); ++index) {
        indices_.emplace(provider.elements[index].id, index);
    }
    state_.assign(provider.elements.size(), 0);
    transforms_.resize(provider.elements.size());
    opacities_.resize(provider.elements.size());
    clips_.resize(provider.elements.size());
    visibility_.resize(provider.elements.size());
    output.reserve(provider.elements.size());

    for (std::size_t index = 0; index < provider.elements.size(); ++index) {
        if (!resolve_element(
            provider,
            index,
            canvas,
            fit->viewport_size,
            canvas_clip)) {
            output.clear();
            return false;
        }
        if (!std::holds_alternative<scene::GroupElement>(provider.elements[index].content)) {
            output.push_back(ResolvedElement {
                &provider.elements[index],
                transforms_[index],
                opacities_[index],
                clips_[index],
                visibility_[index],
            });
        }
    }
    return true;
}

} // namespace mango_overlay::renderer
