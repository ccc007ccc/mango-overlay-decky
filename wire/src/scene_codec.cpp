#include "mango_overlay/wire/scene_codec.hpp"

#include "mango_overlay/scene/validation.hpp"

#include <flatbuffers/verifier.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mango_overlay::wire {

namespace {

scene::Vec2 decode_vec2(const MangoOverlay::Wire::Vec2& value)
{
    return { value.x(), value.y() };
}

scene::Color decode_color(const MangoOverlay::Wire::Color& value)
{
    return { value.red(), value.green(), value.blue(), value.alpha() };
}

std::optional<scene::Visibility> decode_visibility(MangoOverlay::Wire::Visibility visibility)
{
    switch (visibility) {
    case MangoOverlay::Wire::Visibility::GameOnly:
        return scene::Visibility::game_only;
    case MangoOverlay::Wire::Visibility::SteamOnly:
        return scene::Visibility::steam_only;
    case MangoOverlay::Wire::Visibility::Always:
        return scene::Visibility::always;
    }
    return std::nullopt;
}

MangoOverlay::Wire::Visibility encode_visibility(scene::Visibility visibility)
{
    switch (visibility) {
    case scene::Visibility::game_only:
        return MangoOverlay::Wire::Visibility::GameOnly;
    case scene::Visibility::steam_only:
        return MangoOverlay::Wire::Visibility::SteamOnly;
    case scene::Visibility::always:
        return MangoOverlay::Wire::Visibility::Always;
    }
    throw std::invalid_argument("scene has an invalid visibility");
}

std::optional<scene::Anchor> decode_anchor(MangoOverlay::Wire::Anchor anchor)
{
    switch (anchor) {
    case MangoOverlay::Wire::Anchor::TopLeft:
        return scene::Anchor::top_left;
    case MangoOverlay::Wire::Anchor::TopCenter:
        return scene::Anchor::top_center;
    case MangoOverlay::Wire::Anchor::TopRight:
        return scene::Anchor::top_right;
    case MangoOverlay::Wire::Anchor::CenterLeft:
        return scene::Anchor::center_left;
    case MangoOverlay::Wire::Anchor::Center:
        return scene::Anchor::center;
    case MangoOverlay::Wire::Anchor::CenterRight:
        return scene::Anchor::center_right;
    case MangoOverlay::Wire::Anchor::BottomLeft:
        return scene::Anchor::bottom_left;
    case MangoOverlay::Wire::Anchor::BottomCenter:
        return scene::Anchor::bottom_center;
    case MangoOverlay::Wire::Anchor::BottomRight:
        return scene::Anchor::bottom_right;
    }
    return std::nullopt;
}

MangoOverlay::Wire::Anchor encode_anchor(scene::Anchor anchor)
{
    switch (anchor) {
    case scene::Anchor::top_left:
        return MangoOverlay::Wire::Anchor::TopLeft;
    case scene::Anchor::top_center:
        return MangoOverlay::Wire::Anchor::TopCenter;
    case scene::Anchor::top_right:
        return MangoOverlay::Wire::Anchor::TopRight;
    case scene::Anchor::center_left:
        return MangoOverlay::Wire::Anchor::CenterLeft;
    case scene::Anchor::center:
        return MangoOverlay::Wire::Anchor::Center;
    case scene::Anchor::center_right:
        return MangoOverlay::Wire::Anchor::CenterRight;
    case scene::Anchor::bottom_left:
        return MangoOverlay::Wire::Anchor::BottomLeft;
    case scene::Anchor::bottom_center:
        return MangoOverlay::Wire::Anchor::BottomCenter;
    case scene::Anchor::bottom_right:
        return MangoOverlay::Wire::Anchor::BottomRight;
    }
    throw std::invalid_argument("scene has an invalid anchor");
}

bool default_layout(const scene::Element& element)
{
    const auto& layout = element.transform;
    return element.parent_id == 0
        && layout.translation.x == 0.0F && layout.translation.y == 0.0F
        && layout.scale.x == 1.0F && layout.scale.y == 1.0F
        && layout.rotation_degrees == 0.0F && layout.opacity == 1.0F
        && layout.anchor == scene::Anchor::top_left && !layout.clip.has_value()
        && !layout.hidden;
}

flatbuffers::Offset<MangoOverlay::Wire::ElementLayout> encode_layout(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::Element& element)
{
    if (default_layout(element)) {
        return {};
    }
    const MangoOverlay::Wire::Vec2 translation(
        element.transform.translation.x, element.transform.translation.y);
    const MangoOverlay::Wire::Vec2 scale(
        element.transform.scale.x, element.transform.scale.y);
    flatbuffers::Offset<MangoOverlay::Wire::ClipRect> clip;
    if (element.transform.clip.has_value()) {
        const MangoOverlay::Wire::Vec2 position(
            element.transform.clip->position.x,
            element.transform.clip->position.y);
        const MangoOverlay::Wire::Vec2 size(
            element.transform.clip->size.x,
            element.transform.clip->size.y);
        clip = MangoOverlay::Wire::CreateClipRect(builder, &position, &size);
    }
    return MangoOverlay::Wire::CreateElementLayout(
        builder,
        element.parent_id,
        &translation,
        &scale,
        element.transform.rotation_degrees,
        element.transform.opacity,
        encode_anchor(element.transform.anchor),
        clip,
        element.transform.hidden);
}

} // namespace

ElementDecodeResult decode_element(const MangoOverlay::Wire::Element& wire)
{
    scene::ElementContent content;
    switch (wire.content_type()) {
    case MangoOverlay::Wire::ElementContent::TextElement: {
        const auto* text = wire.content_as_TextElement();
        if (text == nullptr || text->position() == nullptr || text->text() == nullptr
            || text->color() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::TextElement {
            decode_vec2(*text->position()),
            text->text()->str(),
            text->font_size(),
            decode_color(*text->color()),
        };
        break;
    }
    case MangoOverlay::Wire::ElementContent::RectangleElement: {
        const auto* rectangle = wire.content_as_RectangleElement();
        if (rectangle == nullptr || rectangle->position() == nullptr
            || rectangle->size() == nullptr || rectangle->color() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::RectangleElement {
            decode_vec2(*rectangle->position()),
            decode_vec2(*rectangle->size()),
            rectangle->corner_radius(),
            decode_color(*rectangle->color()),
        };
        break;
    }
    case MangoOverlay::Wire::ElementContent::LineElement: {
        const auto* line = wire.content_as_LineElement();
        if (line == nullptr || line->start() == nullptr || line->end() == nullptr
            || line->color() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::LineElement {
            decode_vec2(*line->start()),
            decode_vec2(*line->end()),
            line->thickness(),
            decode_color(*line->color()),
        };
        break;
    }
    case MangoOverlay::Wire::ElementContent::PolylineElement: {
        const auto* polyline = wire.content_as_PolylineElement();
        if (polyline == nullptr || polyline->points() == nullptr
            || polyline->color() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        scene::PolylineElement decoded {
            {}, polyline->thickness(), decode_color(*polyline->color()) };
        decoded.points.reserve(polyline->points()->size());
        for (const auto* point : *polyline->points()) {
            if (point == nullptr) {
                return SceneDecodeError::invalid_element;
            }
            decoded.points.push_back(decode_vec2(*point));
        }
        content = std::move(decoded);
        break;
    }
    case MangoOverlay::Wire::ElementContent::CircleElement: {
        const auto* circle = wire.content_as_CircleElement();
        if (circle == nullptr || circle->center() == nullptr
            || circle->color() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::CircleElement {
            decode_vec2(*circle->center()),
            circle->radius(),
            decode_color(*circle->color()),
        };
        break;
    }
    case MangoOverlay::Wire::ElementContent::GroupElement:
        if (wire.content_as_GroupElement() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::GroupElement {};
        break;
    case MangoOverlay::Wire::ElementContent::ImageElement: {
        const auto* image = wire.content_as_ImageElement();
        if (image == nullptr || image->position() == nullptr
            || image->size() == nullptr || image->tint() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::ImageElement {
            decode_vec2(*image->position()),
            decode_vec2(*image->size()),
            image->resource_id(),
            decode_color(*image->tint()),
        };
        break;
    }
    case MangoOverlay::Wire::ElementContent::GifElement: {
        const auto* gif = wire.content_as_GifElement();
        if (gif == nullptr || gif->position() == nullptr
            || gif->size() == nullptr || gif->tint() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        content = scene::GifElement {
            decode_vec2(*gif->position()),
            decode_vec2(*gif->size()),
            gif->resource_id(),
            decode_color(*gif->tint()),
            gif->playback_rate(),
            gif->paused(),
            gif->frame_index(),
        };
        break;
    }
    case MangoOverlay::Wire::ElementContent::NONE:
        return SceneDecodeError::invalid_element;
    }

    scene::Element element { wire.id(), wire.z_index(), std::move(content) };
    const auto* layout = wire.layout();
    if (layout != nullptr) {
        if (layout->translation() == nullptr || layout->scale() == nullptr) {
            return SceneDecodeError::invalid_element;
        }
        const auto anchor = decode_anchor(layout->anchor());
        if (!anchor.has_value()) {
            return SceneDecodeError::invalid_element;
        }
        element.parent_id = layout->parent_id();
        element.transform.translation = decode_vec2(*layout->translation());
        element.transform.scale = decode_vec2(*layout->scale());
        element.transform.rotation_degrees = layout->rotation_degrees();
        element.transform.opacity = layout->opacity();
        element.transform.anchor = *anchor;
        element.transform.hidden = layout->hidden();
        if (layout->clip() != nullptr) {
            if (layout->clip()->position() == nullptr
                || layout->clip()->size() == nullptr) {
                return SceneDecodeError::invalid_element;
            }
            element.transform.clip = scene::ClipRect {
                decode_vec2(*layout->clip()->position()),
                decode_vec2(*layout->clip()->size()),
            };
        }
    }
    return element;
}

std::shared_ptr<const scene::ProviderScene> decode_provider(
    const MangoOverlay::Wire::ProviderScene* wire,
    const scene::SceneLimits& limits)
{
    if (wire == nullptr || wire->application_id() == nullptr
        || wire->instance_id() == nullptr || wire->display_name() == nullptr
        || wire->elements() == nullptr) {
        return nullptr;
    }
    const auto visibility = decode_visibility(wire->requested_visibility());
    if (!visibility.has_value() || wire->elements()->size() > limits.maximum_elements_per_provider) {
        return nullptr;
    }

    scene::ProviderScene provider {
        scene::ProviderIdentity {
            wire->application_id()->str(),
            wire->instance_id()->str(),
            wire->display_name()->str(),
            wire->canvas_width(),
            wire->canvas_height(),
            *visibility,
        },
        {},
    };
    provider.elements.reserve(wire->elements()->size());
    for (const auto* wire_element : *wire->elements()) {
        if (wire_element == nullptr) {
            return nullptr;
        }
        auto decoded = decode_element(*wire_element);
        const auto* element = std::get_if<scene::Element>(&decoded);
        if (element == nullptr) {
            return nullptr;
        }
        provider.elements.push_back(*element);
    }
    if (!scene::valid_provider_scene(provider, limits)) {
        return nullptr;
    }
    std::sort(provider.elements.begin(), provider.elements.end(), [](const auto& left, const auto& right) {
        return left.z_index < right.z_index
            || (left.z_index == right.z_index && left.id < right.id);
    });
    return std::make_shared<const scene::ProviderScene>(std::move(provider));
}

TransactionDecodeResult decode_transaction(protocol::ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return SceneDecodeError::malformed_payload;
    }
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::SceneTransaction>(nullptr)) {
        return SceneDecodeError::malformed_payload;
    }

    const auto* wire = flatbuffers::GetRoot<MangoOverlay::Wire::SceneTransaction>(payload.data);
    if (wire->mutations() == nullptr) {
        return SceneDecodeError::malformed_payload;
    }

    std::vector<scene::SceneMutation> mutations;
    mutations.reserve(wire->mutations()->size());
    for (const auto* wire_mutation : *wire->mutations()) {
        if (wire_mutation == nullptr) {
            return SceneDecodeError::invalid_mutation;
        }
        switch (wire_mutation->content_type()) {
        case MangoOverlay::Wire::MutationContent::UpsertElement: {
            const auto* upsert = wire_mutation->content_as_UpsertElement();
            if (upsert == nullptr || upsert->element() == nullptr) {
                return SceneDecodeError::invalid_mutation;
            }
            auto decoded = decode_element(*upsert->element());
            const auto* element = std::get_if<scene::Element>(&decoded);
            if (element == nullptr) {
                return std::get<SceneDecodeError>(decoded);
            }
            mutations.push_back(scene::UpsertElement { *element });
            break;
        }
        case MangoOverlay::Wire::MutationContent::RemoveElement: {
            const auto* removal = wire_mutation->content_as_RemoveElement();
            if (removal == nullptr) {
                return SceneDecodeError::invalid_mutation;
            }
            mutations.push_back(scene::RemoveElement { removal->element_id() });
            break;
        }
        case MangoOverlay::Wire::MutationContent::NONE:
            return SceneDecodeError::invalid_mutation;
        }
    }
    return scene::SceneTransaction { wire->transaction_id(), std::move(mutations) };
}

flatbuffers::Offset<MangoOverlay::Wire::Element> encode_element(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::Element& element)
{
    const auto layout = encode_layout(builder, element);
    if (std::holds_alternative<scene::GroupElement>(element.content)) {
        const auto group = MangoOverlay::Wire::CreateGroupElement(builder);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::GroupElement,
            group.Union(),
            layout);
    }
    if (const auto* text = std::get_if<scene::TextElement>(&element.content)) {
        const MangoOverlay::Wire::Vec2 position(text->position.x, text->position.y);
        const MangoOverlay::Wire::Color color(
            text->color.red, text->color.green, text->color.blue, text->color.alpha);
        const auto wire_text = MangoOverlay::Wire::CreateTextElement(
            builder,
            &position,
            builder.CreateString(text->text),
            text->font_size,
            &color);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::TextElement,
            wire_text.Union(),
            layout);
    }

    if (const auto* rectangle = std::get_if<scene::RectangleElement>(&element.content)) {
        const MangoOverlay::Wire::Vec2 position(
            rectangle->position.x, rectangle->position.y);
        const MangoOverlay::Wire::Vec2 size(rectangle->size.x, rectangle->size.y);
        const MangoOverlay::Wire::Color color(
            rectangle->color.red,
            rectangle->color.green,
            rectangle->color.blue,
            rectangle->color.alpha);
        const auto wire_rectangle = MangoOverlay::Wire::CreateRectangleElement(
            builder,
            &position,
            &size,
            rectangle->corner_radius,
            &color);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::RectangleElement,
            wire_rectangle.Union(),
            layout);
    }
    if (const auto* line = std::get_if<scene::LineElement>(&element.content)) {
        const MangoOverlay::Wire::Vec2 start(line->start.x, line->start.y);
        const MangoOverlay::Wire::Vec2 end(line->end.x, line->end.y);
        const MangoOverlay::Wire::Color color(
            line->color.red, line->color.green, line->color.blue, line->color.alpha);
        const auto wire_line = MangoOverlay::Wire::CreateLineElement(
            builder, &start, &end, line->thickness, &color);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::LineElement,
            wire_line.Union(),
            layout);
    }
    if (const auto* polyline = std::get_if<scene::PolylineElement>(&element.content)) {
        std::vector<MangoOverlay::Wire::Vec2> points;
        points.reserve(polyline->points.size());
        for (const auto point : polyline->points) {
            points.emplace_back(point.x, point.y);
        }
        const MangoOverlay::Wire::Color color(
            polyline->color.red,
            polyline->color.green,
            polyline->color.blue,
            polyline->color.alpha);
        const auto wire_polyline = MangoOverlay::Wire::CreatePolylineElement(
            builder,
            builder.CreateVectorOfStructs(points),
            polyline->thickness,
            &color);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::PolylineElement,
            wire_polyline.Union(),
            layout);
    }
    if (const auto* circle = std::get_if<scene::CircleElement>(&element.content)) {
        const MangoOverlay::Wire::Vec2 center(circle->center.x, circle->center.y);
        const MangoOverlay::Wire::Color color(
            circle->color.red,
            circle->color.green,
            circle->color.blue,
            circle->color.alpha);
        const auto wire_circle = MangoOverlay::Wire::CreateCircleElement(
            builder, &center, circle->radius, &color);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::CircleElement,
            wire_circle.Union(),
            layout);
    }
    if (const auto* image = std::get_if<scene::ImageElement>(&element.content)) {
        const MangoOverlay::Wire::Vec2 position(image->position.x, image->position.y);
        const MangoOverlay::Wire::Vec2 size(image->size.x, image->size.y);
        const MangoOverlay::Wire::Color tint(
            image->tint.red, image->tint.green, image->tint.blue, image->tint.alpha);
        const auto wire_image = MangoOverlay::Wire::CreateImageElement(
            builder, &position, &size, image->resource_id, &tint);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::ImageElement,
            wire_image.Union(),
            layout);
    }
    if (const auto* gif = std::get_if<scene::GifElement>(&element.content)) {
        const MangoOverlay::Wire::Vec2 position(gif->position.x, gif->position.y);
        const MangoOverlay::Wire::Vec2 size(gif->size.x, gif->size.y);
        const MangoOverlay::Wire::Color tint(
            gif->tint.red, gif->tint.green, gif->tint.blue, gif->tint.alpha);
        const auto wire_gif = MangoOverlay::Wire::CreateGifElement(
            builder,
            &position,
            &size,
            gif->resource_id,
            &tint,
            gif->playback_rate,
            gif->paused,
            gif->frame_index);
        return MangoOverlay::Wire::CreateElement(
            builder,
            element.id,
            element.z_index,
            MangoOverlay::Wire::ElementContent::GifElement,
            wire_gif.Union(),
            layout);
    }
    throw std::invalid_argument("scene has an unknown element type");
}

flatbuffers::Offset<MangoOverlay::Wire::Mutation> encode_mutation(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::SceneMutation& mutation)
{
    if (const auto* upsert = std::get_if<scene::UpsertElement>(&mutation)) {
        const auto wire_element = encode_element(builder, upsert->element);
        const auto wire_upsert = MangoOverlay::Wire::CreateUpsertElement(
            builder, wire_element);
        return MangoOverlay::Wire::CreateMutation(
            builder,
            MangoOverlay::Wire::MutationContent::UpsertElement,
            wire_upsert.Union());
    }
    const auto removal = MangoOverlay::Wire::CreateRemoveElement(
        builder, std::get<scene::RemoveElement>(mutation).id);
    return MangoOverlay::Wire::CreateMutation(
        builder,
        MangoOverlay::Wire::MutationContent::RemoveElement,
        removal.Union());
}

flatbuffers::Offset<MangoOverlay::Wire::ProviderScene> encode_provider(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::ProviderScene& provider)
{
    std::vector<flatbuffers::Offset<MangoOverlay::Wire::Element>> elements;
    elements.reserve(provider.elements.size());
    for (const auto& element : provider.elements) {
        elements.push_back(encode_element(builder, element));
    }
    return MangoOverlay::Wire::CreateProviderScene(
        builder,
        builder.CreateString(provider.identity.application_id),
        builder.CreateString(provider.identity.instance_id),
        builder.CreateString(provider.identity.display_name),
        provider.identity.canvas_width,
        provider.identity.canvas_height,
        encode_visibility(provider.identity.requested_visibility),
        builder.CreateVector(elements));
}

} // namespace mango_overlay::wire
