#include "mango_overlay/wire/scene_codec.hpp"

#include <flatbuffers/flatbuffers.h>

#include <cstdio>
#include <variant>

using mango_overlay::protocol::ByteView;
using mango_overlay::scene::CircleElement;
using mango_overlay::scene::Color;
using mango_overlay::scene::Element;
using mango_overlay::scene::GroupElement;
using mango_overlay::scene::LineElement;
using mango_overlay::scene::PolylineElement;
using mango_overlay::scene::SceneMutation;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::UpsertElement;
using mango_overlay::wire::decode_transaction;
using mango_overlay::wire::encode_mutation;

int main()
{
    const Color white { 1.0F, 1.0F, 1.0F, 1.0F };
    Element group { 4, -1, GroupElement {} };
    group.transform.translation = { 40.0F, 50.0F };
    group.transform.scale = { 1.5F, 0.75F };
    group.transform.rotation_degrees = 12.0F;
    group.transform.opacity = 0.6F;
    group.transform.anchor = mango_overlay::scene::Anchor::bottom_right;
    group.transform.clip = mango_overlay::scene::ClipRect {
        { 2.0F, 3.0F }, { 200.0F, 100.0F } };

    Element child {
        1,
        0,
        LineElement { { 1.0F, 2.0F }, { 3.0F, 4.0F }, 2.0F, white },
    };
    child.parent_id = 4;
    const std::vector<SceneMutation> source {
        UpsertElement { group },
        UpsertElement { child },
        UpsertElement { Element {
            2,
            1,
            PolylineElement {
                { { 5.0F, 6.0F }, { 7.0F, 8.0F }, { 9.0F, 10.0F } },
                3.0F,
                white,
            },
        } },
        UpsertElement { Element {
            3,
            2,
            CircleElement { { 11.0F, 12.0F }, 13.0F, white },
        } },
    };

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<MangoOverlay::Wire::Mutation>> mutations;
    for (const auto& mutation : source) {
        mutations.push_back(encode_mutation(builder, mutation));
    }
    const auto transaction = MangoOverlay::Wire::CreateSceneTransaction(
        builder, 7, builder.CreateVector(mutations));
    builder.Finish(transaction);

    const auto decoded = decode_transaction(
        ByteView { builder.GetBufferPointer(), builder.GetSize() });
    const auto* scene = std::get_if<SceneTransaction>(&decoded);
    if (scene == nullptr || scene->transaction_id != 7 || scene->mutations.size() != 4) {
        std::fputs("primitive transaction did not round trip\n", stderr);
        return 1;
    }
    const auto& decoded_group = std::get<UpsertElement>(scene->mutations[0]).element;
    const auto& decoded_child = std::get<UpsertElement>(scene->mutations[1]).element;
    const auto* line = std::get_if<LineElement>(&decoded_child.content);
    const auto* polyline = std::get_if<PolylineElement>(
        &std::get<UpsertElement>(scene->mutations[2]).element.content);
    const auto* circle = std::get_if<CircleElement>(
        &std::get<UpsertElement>(scene->mutations[3]).element.content);
    if (!std::holds_alternative<GroupElement>(decoded_group.content)
        || decoded_group.transform.translation.x != 40.0F
        || decoded_group.transform.scale.y != 0.75F
        || decoded_group.transform.rotation_degrees != 12.0F
        || decoded_group.transform.opacity != 0.6F
        || decoded_group.transform.anchor
            != mango_overlay::scene::Anchor::bottom_right
        || !decoded_group.transform.clip.has_value()
        || decoded_group.transform.clip->size.x != 200.0F
        || decoded_child.parent_id != 4
        || line == nullptr || line->end.y != 4.0F
        || polyline == nullptr || polyline->points.size() != 3
        || polyline->points[1].x != 7.0F
        || circle == nullptr || circle->radius != 13.0F) {
        std::fputs("primitive transaction lost element data\n", stderr);
        return 1;
    }
    return 0;
}
