#include "mango_overlay/renderer/imgui_scene.hpp"

#include "mango_overlay/renderer/canvas.hpp"
#include "mango_overlay/renderer/layout.hpp"
#include "mango_overlay/renderer/texture_cache.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <variant>
#include <vector>

namespace mango_overlay::renderer {

namespace {

ImVec2 imgui_point(scene::Vec2 value)
{
    return { value.x, value.y };
}

ImU32 imgui_color(scene::Color value, float opacity)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        value.red,
        value.green,
        value.blue,
        std::clamp(value.alpha * opacity, 0.0F, 1.0F)));
}

struct DrawingTransform {
    explicit DrawingTransform(const AffineTransform& configured)
        : target(configured)
        , scale(std::max(
              0.001F,
              std::max(
                  std::hypot(target.xx, target.yx),
                  std::hypot(target.xy, target.yy))))
    {
    }

    scene::Vec2 point(scene::Vec2 value) const
    {
        return { target.x + value.x * scale, target.y + value.y * scale };
    }

    void apply(ImDrawList& draw_list, int first_vertex) const
    {
        for (int index = first_vertex; index < draw_list.VtxBuffer.Size; ++index) {
            const scene::Vec2 local {
                (draw_list.VtxBuffer[index].pos.x - target.x) / scale,
                (draw_list.VtxBuffer[index].pos.y - target.y) / scale,
            };
            draw_list.VtxBuffer[index].pos = imgui_point(target.point(local));
        }
    }

    AffineTransform target;
    float scale;
};

const scene::ImageResource* find_resource(
    const scene::ProviderScene& provider,
    scene::ResourceId resource_id)
{
    const auto resource = std::find_if(
        provider.resources.begin(),
        provider.resources.end(),
        [resource_id](const auto& candidate) {
            return candidate != nullptr && candidate->id == resource_id;
        });
    return resource == provider.resources.end() ? nullptr : resource->get();
}

void draw_image(
    ImDrawList& draw_list,
    const DrawingTransform& transform,
    scene::Vec2 position,
    scene::Vec2 size,
    scene::Color tint,
    float opacity,
    TextureHandle texture)
{
    if (texture == 0) {
        return;
    }
    const scene::Vec2 maximum { position.x + size.x, position.y + size.y };
    draw_list.AddImage(
        static_cast<ImTextureID>(texture),
        imgui_point(transform.point(position)),
        imgui_point(transform.point(maximum)),
        { 0.0F, 0.0F },
        { 1.0F, 1.0F },
        imgui_color(tint, opacity));
}

void draw_element(
    ImDrawList& draw_list,
    ImFont* text_font,
    const scene::ProviderScene& provider,
    const ResolvedElement& resolved,
    const TextureCache& textures,
    std::uint64_t elapsed_ms)
{
    if (!resolved.visible || !resolved.clip.has_value() || resolved.element == nullptr) {
        return;
    }
    const auto& element = *resolved.element;
    const DrawingTransform transform(resolved.transform);
    draw_list.PushClipRect(
        imgui_point(resolved.clip->minimum),
        imgui_point(resolved.clip->maximum),
        true);
    const int first_vertex = draw_list.VtxBuffer.Size;

    if (const auto* text = std::get_if<scene::TextElement>(&element.content)) {
        draw_list.AddText(
            text_font != nullptr ? text_font : ImGui::GetFont(),
            text->font_size * transform.scale,
            imgui_point(transform.point(text->position)),
            imgui_color(text->color, resolved.opacity),
            text->text.c_str());
    } else if (const auto* rectangle
        = std::get_if<scene::RectangleElement>(&element.content)) {
        const scene::Vec2 maximum {
            rectangle->position.x + rectangle->size.x,
            rectangle->position.y + rectangle->size.y,
        };
        draw_list.AddRectFilled(
            imgui_point(transform.point(rectangle->position)),
            imgui_point(transform.point(maximum)),
            imgui_color(rectangle->color, resolved.opacity),
            rectangle->corner_radius * transform.scale);
    } else if (const auto* line = std::get_if<scene::LineElement>(&element.content)) {
        draw_list.AddLine(
            imgui_point(transform.point(line->start)),
            imgui_point(transform.point(line->end)),
            imgui_color(line->color, resolved.opacity),
            line->thickness * transform.scale);
    } else if (const auto* polyline
        = std::get_if<scene::PolylineElement>(&element.content)) {
        for (std::size_t index = 1; index < polyline->points.size(); ++index) {
            draw_list.AddLine(
                imgui_point(transform.point(polyline->points[index - 1])),
                imgui_point(transform.point(polyline->points[index])),
                imgui_color(polyline->color, resolved.opacity),
                polyline->thickness * transform.scale);
        }
    } else if (const auto* circle = std::get_if<scene::CircleElement>(&element.content)) {
        draw_list.AddCircleFilled(
            imgui_point(transform.point(circle->center)),
            circle->radius * transform.scale,
            imgui_color(circle->color, resolved.opacity));
    } else if (const auto* image = std::get_if<scene::ImageElement>(&element.content)) {
        draw_image(
            draw_list,
            transform,
            image->position,
            image->size,
            image->tint,
            resolved.opacity,
            textures.frame(provider, image->resource_id, 0));
    } else if (const auto* gif = std::get_if<scene::GifElement>(&element.content)) {
        const auto* resource = find_resource(provider, gif->resource_id);
        if (resource != nullptr) {
            const auto frame = gif_frame_at(
                resource->decoded,
                elapsed_ms,
                gif->playback_rate,
                gif->paused,
                gif->frame_index);
            draw_image(
                draw_list,
                transform,
                gif->position,
                gif->size,
                gif->tint,
                resolved.opacity,
                textures.frame(provider, gif->resource_id, frame));
        }
    }

    transform.apply(draw_list, first_vertex);
    draw_list.PopClipRect();
}

} // namespace

struct ImGuiSceneRenderer::Impl {
    explicit Impl(std::unique_ptr<TextureBackend> configured_texture_backend)
        : texture_backend(std::move(configured_texture_backend))
    {
    }

    ~Impl()
    {
        if (texture_backend) {
            textures.clear(*texture_backend);
        }
    }

    TextureCache textures;
    std::unique_ptr<TextureBackend> texture_backend;
    LayoutResolver resolver;
    std::vector<ResolvedElement> resolved;
    ImFont* text_font = nullptr;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

ImGuiSceneRenderer::ImGuiSceneRenderer(
    std::unique_ptr<TextureBackend> texture_backend)
    : impl_(std::make_unique<Impl>(std::move(texture_backend)))
{
}

ImGuiSceneRenderer::~ImGuiSceneRenderer() = default;

void ImGuiSceneRenderer::set_text_font(ImFont* font)
{
    impl_->text_font = font;
}

void ImGuiSceneRenderer::draw(
    const scene::SceneSnapshot& snapshot,
    float output_width,
    float output_height,
    bool steam_focused)
{
    if (impl_->texture_backend) {
        impl_->textures.synchronize(snapshot, *impl_->texture_backend);
    }
    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - impl_->started)
            .count());
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    for (const auto& provider : snapshot.providers) {
        if (provider == nullptr
            || !visible_for_focus(provider->identity.requested_visibility, steam_focused)
            || !impl_->resolver.resolve(
                *provider, output_width, output_height, impl_->resolved)) {
            continue;
        }
        for (const auto& element : impl_->resolved) {
            draw_element(
                *draw_list,
                impl_->text_font,
                *provider,
                element,
                impl_->textures,
                elapsed_ms);
        }
    }
}

} // namespace mango_overlay::renderer
