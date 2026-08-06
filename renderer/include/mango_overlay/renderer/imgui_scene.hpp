#pragma once

#include "mango_overlay/scene/store.hpp"
#include "mango_overlay/renderer/texture_cache.hpp"

#include <memory>

struct ImFont;

namespace mango_overlay::renderer {

class ImGuiSceneRenderer {
public:
    explicit ImGuiSceneRenderer(
        std::unique_ptr<TextureBackend> texture_backend = {});
    ~ImGuiSceneRenderer();

    ImGuiSceneRenderer(const ImGuiSceneRenderer&) = delete;
    ImGuiSceneRenderer& operator=(const ImGuiSceneRenderer&) = delete;

    void set_text_font(ImFont* font);

    void draw(
        const scene::SceneSnapshot& snapshot,
        float output_width,
        float output_height,
        bool steam_focused);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mango_overlay::renderer
