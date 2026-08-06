#include "mango_overlay/renderer/layout.hpp"

#include <cmath>
#include <cstdio>
#include <variant>

using namespace mango_overlay::renderer;
using namespace mango_overlay::scene;

namespace {

bool close_to(float left, float right)
{
    return std::fabs(left - right) < 0.001F;
}

const ResolvedElement* find_element(
    const std::vector<ResolvedElement>& elements,
    ElementId id)
{
    for (const auto& element : elements) {
        if (element.element != nullptr && element.element->id == id) {
            return &element;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    Element group { 1, 0, GroupElement {} };
    group.transform.translation = { 100.0F, 50.0F };
    group.transform.scale = { 2.0F, 1.0F };
    group.transform.rotation_degrees = 90.0F;
    group.transform.opacity = 0.5F;
    group.transform.clip = ClipRect { { 0.0F, 0.0F }, { 100.0F, 80.0F } };

    Element child {
        2,
        1,
        RectangleElement {
            { 0.0F, 0.0F },
            { 20.0F, 10.0F },
            0.0F,
            { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
    child.parent_id = 1;
    child.transform.translation = { 10.0F, 0.0F };
    child.transform.opacity = 0.5F;

    Element anchored {
        3,
        2,
        TextElement {
            { 0.0F, 0.0F },
            "anchored",
            20.0F,
            { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
    anchored.transform.anchor = Anchor::bottom_right;
    anchored.transform.translation = { -20.0F, -30.0F };

    ProviderScene provider {
        ProviderIdentity {
            "layout.test", "primary", "Layout", 1280, 800, Visibility::always },
        { group, child, anchored },
    };

    LayoutResolver resolver;
    std::vector<ResolvedElement> resolved;
    if (!resolver.resolve(provider, 1280.0F, 800.0F, resolved)
        || resolved.size() != 2) {
        std::fputs("valid layout did not resolve\n", stderr);
        return 1;
    }

    const auto* resolved_child = find_element(resolved, 2);
    const auto* resolved_anchor = find_element(resolved, 3);
    if (resolved_child == nullptr || resolved_anchor == nullptr) {
        std::fputs("drawable elements were missing from resolved layout\n", stderr);
        return 1;
    }

    const auto child_origin = resolved_child->transform.point({ 0.0F, 0.0F });
    const auto child_x = resolved_child->transform.point({ 1.0F, 0.0F });
    if (!close_to(child_origin.x, 100.0F) || !close_to(child_origin.y, 70.0F)
        || !close_to(child_x.x, 100.0F) || !close_to(child_x.y, 72.0F)
        || !close_to(resolved_child->opacity, 0.25F)
        || !resolved_child->clip.has_value()
        || !close_to(resolved_child->clip->minimum.x, 20.0F)
        || !close_to(resolved_child->clip->minimum.y, 50.0F)
        || !close_to(resolved_child->clip->maximum.x, 100.0F)
        || !close_to(resolved_child->clip->maximum.y, 250.0F)) {
        std::fputs("inherited transform, opacity, or clip is incorrect\n", stderr);
        return 1;
    }

    const auto anchor_origin = resolved_anchor->transform.point({ 0.0F, 0.0F });
    if (!close_to(anchor_origin.x, 1260.0F)
        || !close_to(anchor_origin.y, 770.0F)
        || !close_to(resolved_anchor->opacity, 1.0F)) {
        std::fputs("root canvas anchor is incorrect\n", stderr);
        return 1;
    }

    if (!resolver.resolve(provider, 1920.0F, 1080.0F, resolved)) {
        std::fputs("widescreen layout did not resolve\n", stderr);
        return 1;
    }
    resolved_anchor = find_element(resolved, 3);
    if (resolved_anchor == nullptr || !resolved_anchor->clip.has_value()) {
        std::fputs("widescreen root anchor was not resolved\n", stderr);
        return 1;
    }
    const auto widescreen_anchor
        = resolved_anchor->transform.point({ 0.0F, 0.0F });
    if (!close_to(widescreen_anchor.x, 1893.0F)
        || !close_to(widescreen_anchor.y, 1039.5F)
        || !close_to(resolved_anchor->clip->minimum.x, 0.0F)
        || !close_to(resolved_anchor->clip->maximum.x, 1920.0F)) {
        std::fputs("root anchor did not follow the current output edges\n", stderr);
        return 1;
    }

    group.transform.hidden = true;
    provider.elements[0] = group;
    if (!resolver.resolve(provider, 1280.0F, 800.0F, resolved)) {
        std::fputs("hidden hierarchy did not resolve\n", stderr);
        return 1;
    }
    resolved_child = find_element(resolved, 2);
    if (resolved_child == nullptr || resolved_child->visible) {
        std::fputs("group visibility was not inherited\n", stderr);
        return 1;
    }
    return 0;
}
