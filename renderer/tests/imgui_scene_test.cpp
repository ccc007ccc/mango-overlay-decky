#include "mango_overlay/renderer/imgui_scene.hpp"
#include "mango_overlay/renderer/texture_cache.hpp"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace mango_overlay;

namespace {

struct BackendState {
    std::size_t uploads = 0;
    std::vector<renderer::TextureHandle> destroyed;
};

class FakeTextureBackend final : public renderer::TextureBackend {
public:
    explicit FakeTextureBackend(BackendState& configured_state)
        : state(configured_state)
    {
    }

    renderer::TextureHandle upload_rgba(
        std::uint32_t,
        std::uint32_t,
        const std::uint8_t*,
        std::size_t) override
    {
        ++state.uploads;
        return 73;
    }

    void destroy(renderer::TextureHandle handle) override
    {
        state.destroyed.push_back(handle);
    }

private:
    BackendState& state;
};

scene::SceneSnapshot make_snapshot()
{
    auto image_resource = std::make_shared<const scene::ImageResource>(
        scene::ImageResource {
            9,
            { 1, 2, 3 },
            resource::DecodedImage {
                resource::ImageFormat::png,
                1,
                1,
                { 20 },
                { 255, 0, 0, 255 },
            },
        });
    auto provider = std::make_shared<const scene::ProviderScene>(
        scene::ProviderScene {
            scene::ProviderIdentity {
                "renderer.test",
                "primary",
                "Renderer Test",
                1280,
                800,
                scene::Visibility::always,
            },
            {
                scene::Element {
                    1,
                    0,
                    scene::RectangleElement {
                        { 10.0F, 10.0F },
                        { 120.0F, 80.0F },
                        4.0F,
                        { 0.1F, 0.2F, 0.3F, 1.0F },
                    },
                },
                scene::Element {
                    2,
                    1,
                    scene::ImageElement {
                        { 20.0F, 20.0F },
                        { 32.0F, 32.0F },
                        9,
                        { 1.0F, 1.0F, 1.0F, 1.0F },
                    },
                },
            },
            { std::move(image_resource) },
        });
    return { 1, { std::move(provider) } };
}

bool render_snapshot(renderer::ImGuiSceneRenderer& renderer)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = { 1280.0F, 800.0F };
    io.DeltaTime = 1.0F / 60.0F;
    ImGui::NewFrame();
    renderer.draw(make_snapshot(), 1280.0F, 800.0F, false);
    ImGui::Render();
    return ImGui::GetDrawData() != nullptr
        && ImGui::GetDrawData()->TotalVtxCount > 0;
}

} // namespace

int main()
{
    ImGui::CreateContext();
    ImGui::GetIO().Fonts->AddFontDefault();
    unsigned char* font_pixels = nullptr;
    int font_width = 0;
    int font_height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
        &font_pixels, &font_width, &font_height);

    {
        renderer::ImGuiSceneRenderer renderer_without_textures;
        if (!render_snapshot(renderer_without_textures)) {
            std::fputs("geometry did not render without a texture adapter\n", stderr);
            ImGui::DestroyContext();
            return 1;
        }
    }

    BackendState state;
    {
        renderer::ImGuiSceneRenderer renderer_with_textures(
            std::make_unique<FakeTextureBackend>(state));
        if (!render_snapshot(renderer_with_textures) || state.uploads != 1) {
            std::fputs("texture adapter was not used by the shared renderer\n", stderr);
            ImGui::DestroyContext();
            return 1;
        }
    }
    if (state.destroyed != std::vector<renderer::TextureHandle> { 73 }) {
        std::fputs("texture adapter did not release its owned handle\n", stderr);
        ImGui::DestroyContext();
        return 1;
    }

    ImGui::DestroyContext();
    return 0;
}
