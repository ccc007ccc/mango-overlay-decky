#include "mango_overlay/scene/store.hpp"

#include <cstdio>

using mango_overlay::scene::Color;
using mango_overlay::scene::CommitResult;
using mango_overlay::scene::Element;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneLimits;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

namespace {

Element text_element(std::uint64_t id, const char* text)
{
    return Element {
        id,
        0,
        TextElement {
            Vec2 { 0.0F, 0.0F },
            text,
            18.0F,
            Color { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
}

} // namespace

int main()
{
    SceneLimits limits;
    limits.maximum_scene_bytes_per_provider = 200;
    SceneStore store(limits);
    store.register_provider(
        1,
        ProviderIdentity {
            "size.limit", "primary", "Size Limit", 1280, 800, Visibility::game_only });

    const auto initial = store.commit(
        1,
        SceneTransaction { 1, { UpsertElement { text_element(1, "first") } } });
    const auto rejected = store.commit(
        1,
        SceneTransaction { 2, { UpsertElement { text_element(2, "second") } } });
    const auto snapshot = store.snapshot();
    if (initial != CommitResult::applied
        || rejected != CommitResult::scene_size_limit_reached
        || snapshot->revision != 2 || snapshot->providers.size() != 1
        || snapshot->providers[0]->elements.size() != 1) {
        std::fputs("scene size limit did not reject the transaction atomically\n", stderr);
        return 1;
    }
    return 0;
}
