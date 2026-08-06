#include "mango_overlay/scene/store.hpp"

#include <cstdio>
#include <string>
#include <variant>

using mango_overlay::scene::Color;
using mango_overlay::scene::CommitResult;
using mango_overlay::scene::Element;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::RegistrationResult;
using mango_overlay::scene::SceneLimits;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

namespace {

Element text_element(std::uint64_t id, std::string text)
{
    return Element {
        id,
        0,
        TextElement {
            Vec2 { 10.0F, 20.0F },
            std::move(text),
            20.0F,
            Color { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
}

} // namespace

int main()
{
    SceneLimits limits;
    limits.maximum_text_bytes = 8;
    SceneStore store(limits);

    const auto registered = store.register_provider(
        1,
        ProviderIdentity {
            "example.telemetry", "primary", "Telemetry", 1280, 800, Visibility::game_only });
    if (registered != RegistrationResult::registered) {
        std::fputs("valid provider registration failed\n", stderr);
        return 1;
    }

    const auto initial = store.commit(
        1,
        SceneTransaction { 1, { UpsertElement { text_element(10, "old") } } });
    if (initial != CommitResult::applied) {
        std::fputs("initial scene transaction failed\n", stderr);
        return 1;
    }

    const auto rejected = store.commit(
        1,
        SceneTransaction {
            2,
            {
                UpsertElement { text_element(10, "new") },
                UpsertElement { text_element(11, "too-long-text") },
            },
        });
    if (rejected != CommitResult::invalid_element) {
        std::fputs("transaction containing an invalid element was not rejected\n", stderr);
        return 1;
    }

    const auto snapshot = store.snapshot();
    if (snapshot->providers.size() != 1 || snapshot->providers[0]->elements.size() != 1) {
        std::fputs("rejected transaction changed the visible scene shape\n", stderr);
        return 1;
    }
    const auto* text = std::get_if<TextElement>(&snapshot->providers[0]->elements[0].content);
    if (text == nullptr || text->text != "old") {
        std::fputs("rejected transaction leaked a partial scene update\n", stderr);
        return 1;
    }

    return 0;
}
