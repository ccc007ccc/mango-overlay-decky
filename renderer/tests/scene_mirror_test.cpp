#include "mango_overlay/broker/snapshot_encoder.hpp"
#include "mango_overlay/renderer/scene_mirror.hpp"
#include "mango_overlay/scene/store.hpp"

#include <cstdio>
#include <variant>

using mango_overlay::broker::encode_scene_change_packets;
using mango_overlay::broker::encode_snapshot_packets;
using mango_overlay::broker::OutboundPacket;
using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ProtocolVersion;
using mango_overlay::renderer::ApplyResult;
using mango_overlay::renderer::SceneMirror;
using mango_overlay::scene::Color;
using mango_overlay::scene::Element;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::SceneTransaction;
using mango_overlay::scene::TextElement;
using mango_overlay::scene::UpsertElement;
using mango_overlay::scene::Vec2;
using mango_overlay::scene::Visibility;

namespace {

SceneTransaction text_update(std::uint64_t transaction_id, const char* text)
{
    return SceneTransaction {
        transaction_id,
        { UpsertElement { Element {
            1,
            0,
            TextElement {
                Vec2 { 20.0F, 30.0F },
                text,
                20.0F,
                Color { 1.0F, 1.0F, 1.0F, 1.0F },
            },
        } } },
    };
}

ApplyResult apply(SceneMirror& mirror, const OutboundPacket& packet)
{
    return mirror.apply_packet(ByteView { packet.bytes.data(), packet.bytes.size() });
}

const TextElement* published_text(const SceneMirror& mirror)
{
    const auto snapshot = mirror.snapshot();
    if (snapshot->providers.size() != 1 || snapshot->providers[0]->elements.size() != 1) {
        return nullptr;
    }
    return std::get_if<TextElement>(&snapshot->providers[0]->elements[0].content);
}

} // namespace

int main()
{
    SceneStore source;
    source.register_provider(
        1,
        ProviderIdentity {
            "mirror.test", "primary", "Mirror Test", 1280, 800, Visibility::always });
    source.commit(1, text_update(1, "initial"));

    SceneMirror mirror(ProtocolVersion { 1, 0 });
    const auto initial = encode_snapshot_packets(
        *source.snapshot(), ProtocolVersion { 1, 0 }, 50);
    if (apply(mirror, initial[0]) != ApplyResult::accepted
        || apply(mirror, initial[1]) != ApplyResult::accepted
        || mirror.snapshot()->revision != 0
        || apply(mirror, initial[2]) != ApplyResult::published) {
        std::fputs("renderer exposed a partial initial snapshot\n", stderr);
        return 1;
    }
    const auto* initial_text = published_text(mirror);
    if (mirror.snapshot()->revision != 2 || initial_text == nullptr
        || initial_text->text != "initial") {
        std::fputs("renderer initial snapshot lost scene content\n", stderr);
        return 1;
    }

    source.commit(1, text_update(2, "live"));
    const auto live_change = source.changes_after(2).changes[0];
    const auto live_packets = encode_scene_change_packets(
        live_change, ProtocolVersion { 1, 0 });
    if (live_packets.size() != 1
        || apply(mirror, live_packets[0]) != ApplyResult::published
        || mirror.snapshot()->revision != 3
        || published_text(mirror)->text != "live") {
        std::fputs("renderer did not atomically publish a live update\n", stderr);
        return 1;
    }

    source.commit(1, text_update(3, "missed"));
    source.commit(1, text_update(4, "gap"));
    const auto gap_change = source.changes_after(4).changes[0];
    const auto gap_packets = encode_scene_change_packets(
        gap_change, ProtocolVersion { 1, 0 });
    if (gap_packets.size() != 1
        || apply(mirror, gap_packets[0]) != ApplyResult::revision_gap
        || mirror.snapshot()->revision != 3
        || published_text(mirror)->text != "live") {
        std::fputs("renderer applied an update across a revision gap\n", stderr);
        return 1;
    }

    const auto resync = encode_snapshot_packets(
        *source.snapshot(), ProtocolVersion { 1, 0 }, 51);
    for (const auto& packet : resync) {
        apply(mirror, packet);
    }
    if (mirror.snapshot()->revision != 5 || published_text(mirror)->text != "gap") {
        std::fputs("renderer did not recover from a revision gap\n", stderr);
        return 1;
    }

    source.disconnect(1);
    const auto removal = source.changes_after(5).changes[0];
    const auto removal_packets = encode_scene_change_packets(
        removal, ProtocolVersion { 1, 0 });
    if (removal_packets.size() != 1
        || apply(mirror, removal_packets[0]) != ApplyResult::published
        || mirror.snapshot()->revision != 6
        || !mirror.snapshot()->providers.empty()) {
        std::fputs("renderer did not remove a disconnected provider\n", stderr);
        return 1;
    }
    return 0;
}
