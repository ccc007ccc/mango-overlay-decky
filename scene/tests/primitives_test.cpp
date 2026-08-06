#include "mango_overlay/scene/store.hpp"

#include <cstdio>

using mango_overlay::scene::CircleElement;
using mango_overlay::scene::Color;
using mango_overlay::scene::CommitResult;
using mango_overlay::scene::Element;
using mango_overlay::scene::LineElement;
using mango_overlay::scene::PolylineElement;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

int main()
{
    SceneStore store;
    store.register_provider(
        1,
        ProviderIdentity {
            "primitives.test", "primary", "Primitives", 1280, 800, Visibility::always });

    const Color white { 1.0F, 1.0F, 1.0F, 1.0F };
    const auto initial = store.commit(
        1,
        SceneTransaction {
            1,
            {
                UpsertElement { Element {
                    1,
                    0,
                    LineElement { { 10.0F, 20.0F }, { 100.0F, 80.0F }, 3.0F, white },
                } },
                UpsertElement { Element {
                    2,
                    1,
                    PolylineElement {
                        { { 20.0F, 30.0F }, { 60.0F, 50.0F }, { 100.0F, 25.0F } },
                        2.0F,
                        white,
                    },
                } },
                UpsertElement { Element {
                    3,
                    2,
                    CircleElement { { 240.0F, 160.0F }, 32.0F, white },
                } },
            },
        });
    if (initial != CommitResult::applied
        || store.snapshot()->providers[0]->elements.size() != 3) {
        std::fputs("valid primitive elements were rejected\n", stderr);
        return 1;
    }

    const auto rejected = store.commit(
        1,
        SceneTransaction {
            2,
            { UpsertElement { Element {
                4,
                3,
                LineElement { { 0.0F, 0.0F }, { 10.0F, 10.0F }, 0.0F, white },
            } } },
        });
    if (rejected != CommitResult::invalid_element
        || store.snapshot()->providers[0]->elements.size() != 3) {
        std::fputs("invalid primitive changed the committed scene\n", stderr);
        return 1;
    }
    return 0;
}
