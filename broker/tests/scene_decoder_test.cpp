#include "mango_overlay/broker/scene_decoder.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>

#include <cstdio>
#include <variant>

using mango_overlay::broker::decode_scene_transaction;
using mango_overlay::protocol::ByteView;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;

int main()
{
    flatbuffers::FlatBufferBuilder builder;
    const MangoOverlay::Wire::Vec2 position(12.5F, 30.0F);
    const MangoOverlay::Wire::Color color(1.0F, 0.5F, 0.25F, 0.75F);
    const auto text_value = builder.CreateString("Hello overlay");
    const auto text = MangoOverlay::Wire::CreateTextElement(
        builder, &position, text_value, 24.0F, &color);
    const auto element = MangoOverlay::Wire::CreateElement(
        builder,
        17,
        -2,
        MangoOverlay::Wire::ElementContent::TextElement,
        text.Union());
    const auto upsert = MangoOverlay::Wire::CreateUpsertElement(builder, element);
    const auto mutation = MangoOverlay::Wire::CreateMutation(
        builder,
        MangoOverlay::Wire::MutationContent::UpsertElement,
        upsert.Union());
    const auto mutations = builder.CreateVector(&mutation, 1);
    const auto transaction = MangoOverlay::Wire::CreateSceneTransaction(builder, 9, mutations);
    builder.Finish(transaction);

    const auto result = decode_scene_transaction(
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
    const auto* decoded = std::get_if<SceneTransaction>(&result);
    if (decoded == nullptr || decoded->transaction_id != 9 || decoded->mutations.size() != 1) {
        std::fputs("valid scene transaction was rejected\n", stderr);
        return 1;
    }

    const auto* decoded_upsert = std::get_if<UpsertElement>(&decoded->mutations[0]);
    if (decoded_upsert == nullptr || decoded_upsert->element.id != 17
        || decoded_upsert->element.z_index != -2) {
        std::fputs("decoded upsert metadata is incorrect\n", stderr);
        return 1;
    }
    const auto* decoded_text = std::get_if<TextElement>(&decoded_upsert->element.content);
    const bool matches = decoded_text != nullptr
        && decoded_text->position.x == 12.5F
        && decoded_text->position.y == 30.0F
        && decoded_text->text == "Hello overlay"
        && decoded_text->font_size == 24.0F
        && decoded_text->color.alpha == 0.75F;
    if (!matches) {
        std::fputs("decoded text element is incorrect\n", stderr);
        return 1;
    }

    return 0;
}
