#include "mango_overlay/renderer/canvas.hpp"

#include <cmath>
#include <cstdio>

using mango_overlay::renderer::fit_canvas;
using mango_overlay::renderer::snapshot_has_visible_provider;
using mango_overlay::renderer::visible_for_focus;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

namespace {

bool close_to(float left, float right)
{
    return std::fabs(left - right) < 0.001F;
}

} // namespace

int main()
{
    const auto widescreen = fit_canvas(1280, 800, 1920.0F, 1080.0F);
    if (!widescreen.has_value() || !close_to(widescreen->scale, 1.35F)
        || !close_to(widescreen->viewport_size.x, 1422.2222F)
        || !close_to(widescreen->viewport_size.y, 800.0F)) {
        std::fputs("widescreen viewport did not preserve reference density\n", stderr);
        return 1;
    }
    const auto mapped = widescreen->point(Vec2 { 100.0F, 200.0F });
    if (!close_to(mapped.x, 135.0F) || !close_to(mapped.y, 270.0F)) {
        std::fputs("canvas point transform is incorrect\n", stderr);
        return 1;
    }
    const auto portrait = fit_canvas(1280, 800, 1024.0F, 1280.0F);
    if (!portrait.has_value() || !close_to(portrait->scale, 0.8F)
        || !close_to(portrait->viewport_size.x, 1280.0F)
        || !close_to(portrait->viewport_size.y, 1600.0F)) {
        std::fputs("portrait viewport did not preserve reference density\n", stderr);
        return 1;
    }
    if (fit_canvas(0, 800, 1920.0F, 1080.0F).has_value()) {
        std::fputs("invalid canvas dimensions produced a transform\n", stderr);
        return 1;
    }
    if (visible_for_focus(Visibility::game_only, true)
        || !visible_for_focus(Visibility::game_only, false)
        || !visible_for_focus(Visibility::steam_only, true)
        || visible_for_focus(Visibility::steam_only, false)
        || !visible_for_focus(Visibility::always, true)) {
        std::fputs("provider visibility did not follow focus policy\n", stderr);
        return 1;
    }
    const mango_overlay::scene::SceneSnapshot empty_snapshot { 0, {} };
    if (snapshot_has_visible_provider(empty_snapshot, false)) {
        std::fputs("empty snapshot was reported as visible\n", stderr);
        return 1;
    }
    return 0;
}
