#include "mango_overlay/scene/store.hpp"

#include <cstdio>

using mango_overlay::scene::Color;
using mango_overlay::scene::Element;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneChangeKind;
using mango_overlay::scene::SceneLimits;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

int main()
{
    SceneLimits limits;
    limits.maximum_retained_changes = 2;
    SceneStore store(limits);

    store.register_provider(
        1,
        ProviderIdentity {
            "change.feed", "primary", "Change Feed", 1280, 800, Visibility::game_only });
    store.commit(
        1,
        SceneTransaction {
            1,
            { UpsertElement { Element {
                10,
                0,
                TextElement {
                    Vec2 { 10.0F, 20.0F },
                    "updated",
                    18.0F,
                    Color { 1.0F, 1.0F, 1.0F, 1.0F },
                },
            } } },
        });

    const auto initial_changes = store.changes_after(0);
    if (initial_changes.history_gap || initial_changes.changes.size() != 2
        || initial_changes.changes[0].revision != 1
        || initial_changes.changes[0].kind != SceneChangeKind::upsert
        || initial_changes.changes[1].revision != 2
        || initial_changes.changes[1].provider == nullptr
        || initial_changes.changes[1].provider->elements.size() != 1) {
        std::fputs("scene change feed did not preserve ordered provider updates\n", stderr);
        return 1;
    }

    store.disconnect(1);
    const auto removal = store.changes_after(2);
    if (removal.history_gap || removal.changes.size() != 1
        || removal.changes[0].revision != 3
        || removal.changes[0].kind != SceneChangeKind::remove_provider
        || removal.changes[0].identity.application_id != "change.feed"
        || removal.changes[0].provider != nullptr) {
        std::fputs("scene change feed did not publish provider removal\n", stderr);
        return 1;
    }

    const auto stale_reader = store.changes_after(0);
    if (!stale_reader.history_gap || !stale_reader.changes.empty()) {
        std::fputs("scene change feed did not detect truncated history\n", stderr);
        return 1;
    }
    return 0;
}
