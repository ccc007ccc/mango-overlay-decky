#include "mango_overlay/scene/validation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace mango_overlay::scene {

namespace {

bool valid_identifier(const std::string& identifier)
{
    if (identifier.empty() || identifier.size() > 128) {
        return false;
    }
    return std::all_of(identifier.begin(), identifier.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_' || character == '-';
    });
}

bool finite(Vec2 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool valid_color(Color color)
{
    return std::isfinite(color.red) && color.red >= 0.0F && color.red <= 1.0F
        && std::isfinite(color.green) && color.green >= 0.0F && color.green <= 1.0F
        && std::isfinite(color.blue) && color.blue >= 0.0F && color.blue <= 1.0F
        && std::isfinite(color.alpha) && color.alpha >= 0.0F && color.alpha <= 1.0F;
}

bool valid_thickness(float thickness)
{
    return std::isfinite(thickness) && thickness > 0.0F && thickness <= 512.0F;
}

bool valid_anchor(Anchor anchor)
{
    return anchor == Anchor::top_left || anchor == Anchor::top_center
        || anchor == Anchor::top_right || anchor == Anchor::center_left
        || anchor == Anchor::center || anchor == Anchor::center_right
        || anchor == Anchor::bottom_left || anchor == Anchor::bottom_center
        || anchor == Anchor::bottom_right;
}

bool valid_transform(const ElementTransform& transform)
{
    const bool valid_clip = !transform.clip.has_value()
        || (finite(transform.clip->position) && finite(transform.clip->size)
            && transform.clip->size.x > 0.0F && transform.clip->size.y > 0.0F);
    return finite(transform.translation) && finite(transform.scale)
        && transform.scale.x > 0.0F && transform.scale.x <= 64.0F
        && transform.scale.y > 0.0F && transform.scale.y <= 64.0F
        && std::isfinite(transform.rotation_degrees)
        && std::isfinite(transform.opacity)
        && transform.opacity >= 0.0F && transform.opacity <= 1.0F
        && valid_anchor(transform.anchor) && valid_clip;
}

} // namespace

bool valid_provider_identity(const ProviderIdentity& identity)
{
    const bool valid_visibility = identity.requested_visibility == Visibility::game_only
        || identity.requested_visibility == Visibility::steam_only
        || identity.requested_visibility == Visibility::always;
    return valid_visibility
        && valid_identifier(identity.application_id)
        && valid_identifier(identity.instance_id)
        && !identity.display_name.empty()
        && identity.display_name.size() <= 128
        && identity.display_name.find('\0') == std::string::npos
        && identity.canvas_width > 0 && identity.canvas_width <= 8192
        && identity.canvas_height > 0 && identity.canvas_height <= 8192;
}

bool valid_scene_element(const Element& element, const SceneLimits& limits)
{
    if (element.id == 0 || element.parent_id == element.id
        || !valid_transform(element.transform)) {
        return false;
    }
    if (std::holds_alternative<GroupElement>(element.content)) {
        return true;
    }
    if (const auto* text = std::get_if<TextElement>(&element.content)) {
        return finite(text->position)
            && !text->text.empty()
            && text->text.size() <= limits.maximum_text_bytes
            && text->text.find('\0') == std::string::npos
            && std::isfinite(text->font_size)
            && text->font_size >= 1.0F && text->font_size <= 512.0F
            && valid_color(text->color);
    }

    if (const auto* rectangle = std::get_if<RectangleElement>(&element.content)) {
        return finite(rectangle->position)
            && finite(rectangle->size)
            && rectangle->size.x > 0.0F && rectangle->size.y > 0.0F
            && std::isfinite(rectangle->corner_radius)
            && rectangle->corner_radius >= 0.0F
            && rectangle->corner_radius
                <= std::min(rectangle->size.x, rectangle->size.y) / 2.0F
            && valid_color(rectangle->color);
    }
    if (const auto* line = std::get_if<LineElement>(&element.content)) {
        return finite(line->start) && finite(line->end)
            && valid_thickness(line->thickness) && valid_color(line->color);
    }
    if (const auto* polyline = std::get_if<PolylineElement>(&element.content)) {
        return polyline->points.size() >= 2
            && polyline->points.size() <= limits.maximum_polyline_points
            && std::all_of(polyline->points.begin(), polyline->points.end(), finite)
            && valid_thickness(polyline->thickness) && valid_color(polyline->color);
    }
    if (const auto* circle = std::get_if<CircleElement>(&element.content)) {
        return finite(circle->center) && std::isfinite(circle->radius)
            && circle->radius > 0.0F && circle->radius <= 8192.0F
            && valid_color(circle->color);
    }
    if (const auto* image = std::get_if<ImageElement>(&element.content)) {
        return finite(image->position) && finite(image->size)
            && image->size.x > 0.0F && image->size.y > 0.0F
            && image->resource_id != 0 && valid_color(image->tint);
    }
    if (const auto* gif = std::get_if<GifElement>(&element.content)) {
        return finite(gif->position) && finite(gif->size)
            && gif->size.x > 0.0F && gif->size.y > 0.0F
            && gif->resource_id != 0 && valid_color(gif->tint)
            && std::isfinite(gif->playback_rate)
            && gif->playback_rate >= 0.1F && gif->playback_rate <= 8.0F;
    }
    return false;
}

bool valid_image_resource(const ImageResource& image, const SceneLimits& limits)
{
    const auto& decoded = image.decoded;
    if (image.id == 0 || image.encoded.empty()
        || image.encoded.size() > limits.maximum_encoded_resource_bytes
        || decoded.width == 0 || decoded.height == 0
        || decoded.frame_count() == 0
        || decoded.frame_count() > 120
        || decoded.width > 4096 || decoded.height > 4096) {
        return false;
    }
    const std::size_t frame_stride = decoded.frame_stride();
    if (frame_stride == 0
        || decoded.frame_count() > limits.maximum_decoded_resource_bytes / frame_stride
        || decoded.rgba.size() != frame_stride * decoded.frame_count()) {
        return false;
    }
    const bool valid_format_frames = decoded.format == resource::ImageFormat::gif
        || decoded.frame_count() == 1;
    if (!valid_format_frames) {
        return false;
    }
    for (const auto duration : decoded.frame_durations_ms) {
        if ((decoded.format == resource::ImageFormat::gif && duration == 0)
            || duration > 60000) {
            return false;
        }
    }
    return true;
}

bool provider_scene_fits_size_limit(
    const ProviderScene& provider,
    const SceneLimits& limits)
{
    constexpr std::size_t estimated_element_overhead = 128;
    std::size_t estimated_size = 0;
    for (const auto& element : provider.elements) {
        std::size_t element_size = estimated_element_overhead;
        if (const auto* text = std::get_if<TextElement>(&element.content)) {
            element_size += text->text.size();
        } else if (const auto* polyline = std::get_if<PolylineElement>(&element.content)) {
            element_size += polyline->points.size() * sizeof(Vec2);
        }
        if (estimated_size > limits.maximum_scene_bytes_per_provider
            || element_size > limits.maximum_scene_bytes_per_provider - estimated_size) {
            return false;
        }
        estimated_size += element_size;
    }
    return true;
}

bool valid_provider_scene(const ProviderScene& provider, const SceneLimits& limits)
{
    if (!valid_provider_identity(provider.identity)
        || provider.elements.size() > limits.maximum_elements_per_provider
        || !provider_scene_fits_size_limit(provider, limits)) {
        return false;
    }

    std::unordered_map<ElementId, std::size_t> element_indices;
    element_indices.reserve(provider.elements.size());
    for (std::size_t index = 0; index < provider.elements.size(); ++index) {
        const auto& element = provider.elements[index];
        if (!valid_scene_element(element, limits)
            || !element_indices.emplace(element.id, index).second) {
            return false;
        }
    }

    std::unordered_map<ElementId, std::size_t> child_counts;
    for (const auto& element : provider.elements) {
        if (element.parent_id == 0) {
            continue;
        }
        if (element.transform.anchor != Anchor::top_left) {
            return false;
        }
        const auto parent = element_indices.find(element.parent_id);
        if (parent == element_indices.end()
            || !std::holds_alternative<GroupElement>(
                provider.elements[parent->second].content)) {
            return false;
        }
        auto& child_count = child_counts[element.parent_id];
        ++child_count;
        if (child_count > limits.maximum_children_per_group) {
            return false;
        }
    }

    std::vector<std::uint8_t> state(provider.elements.size(), 0);
    std::vector<std::size_t> depths(provider.elements.size(), 0);
    std::function<bool(std::size_t)> visit = [&](std::size_t index) {
        if (state[index] == 1) {
            return false;
        }
        if (state[index] == 2) {
            return true;
        }
        state[index] = 1;
        const auto parent_id = provider.elements[index].parent_id;
        if (parent_id == 0) {
            depths[index] = 1;
        } else {
            const auto parent = element_indices.find(parent_id);
            if (parent == element_indices.end() || !visit(parent->second)) {
                return false;
            }
            depths[index] = depths[parent->second] + 1;
        }
        if (depths[index] > limits.maximum_group_depth) {
            return false;
        }
        state[index] = 2;
        return true;
    };
    for (std::size_t index = 0; index < provider.elements.size(); ++index) {
        if (!visit(index)) {
            return false;
        }
    }
    return true;
}

} // namespace mango_overlay::scene
