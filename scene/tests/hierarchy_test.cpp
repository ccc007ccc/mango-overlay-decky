#include "mango_overlay/scene/store.hpp"

#include <cstdio>

using namespace mango_overlay::scene;

namespace {

Element group(ElementId id, ElementId parent = 0)
{
    Element element { id, 0, GroupElement {} };
    element.parent_id = parent;
    return element;
}

Element rectangle(ElementId id, ElementId parent = 0)
{
    Element element {
        id,
        1,
        RectangleElement {
            { 0.0F, 0.0F },
            { 20.0F, 10.0F },
            0.0F,
            { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
    element.parent_id = parent;
    return element;
}

} // namespace

int main()
{
    SceneLimits limits;
    limits.maximum_group_depth = 3;
    limits.maximum_children_per_group = 2;
    SceneStore store(limits);
    if (store.register_provider(
            1,
            ProviderIdentity {
                "hierarchy.test", "primary", "Hierarchy", 1280, 800, Visibility::always })
        != RegistrationResult::registered) {
        std::fputs("provider registration failed\n", stderr);
        return 1;
    }

    const auto initial = store.commit(
        1,
        SceneTransaction {
            1,
            {
                UpsertElement { group(1) },
                UpsertElement { group(2, 1) },
                UpsertElement { rectangle(3, 2) },
            },
        });
    if (initial != CommitResult::applied) {
        std::fputs("valid group hierarchy was rejected\n", stderr);
        return 1;
    }

    const auto missing_parent = store.commit(
        1,
        SceneTransaction { 2, { UpsertElement { rectangle(4, 99) } } });
    const auto non_group_parent = store.commit(
        1,
        SceneTransaction { 3, { UpsertElement { rectangle(4, 3) } } });
    const auto cycle = store.commit(
        1,
        SceneTransaction { 4, { UpsertElement { group(1, 2) } } });
    const auto remove_live_group = store.commit(
        1,
        SceneTransaction { 5, { RemoveElement { 2 } } });
    if (missing_parent != CommitResult::invalid_element
        || non_group_parent != CommitResult::invalid_element
        || cycle != CommitResult::invalid_element
        || remove_live_group != CommitResult::invalid_element
        || store.snapshot()->providers[0]->elements.size() != 3) {
        std::fputs("an invalid hierarchy changed the retained scene\n", stderr);
        return 1;
    }

    const auto too_deep = store.commit(
        1,
        SceneTransaction {
            6,
            {
                UpsertElement { group(4, 2) },
                UpsertElement { rectangle(5, 4) },
            },
        });
    if (too_deep != CommitResult::invalid_element) {
        std::fputs("group depth limit was not enforced\n", stderr);
        return 1;
    }

    const auto too_many_children = store.commit(
        1,
        SceneTransaction {
            7,
            {
                UpsertElement { rectangle(4, 1) },
                UpsertElement { rectangle(5, 1) },
            },
        });
    if (too_many_children != CommitResult::invalid_element) {
        std::fputs("group child limit was not enforced\n", stderr);
        return 1;
    }

    const auto remove_subtree = store.commit(
        1,
        SceneTransaction {
            8,
            {
                RemoveElement { 3 },
                RemoveElement { 2 },
            },
        });
    if (remove_subtree != CommitResult::applied
        || store.snapshot()->providers[0]->elements.size() != 1) {
        std::fputs("atomic subtree removal failed\n", stderr);
        return 1;
    }
    return 0;
}
