#include "mango_overlay/broker/snapshot_encoder.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay/renderer/scene_mirror.hpp"
#include "mango_overlay/resource/image.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <png.h>

#include <cstdio>
#include <memory>
#include <variant>
#include <vector>

using namespace mango_overlay;

namespace {

std::vector<std::uint8_t> make_png()
{
    const std::uint8_t pixels[] = {
        255, 0, 0, 255,
        0, 255, 0, 255,
    };
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    image.width = 2;
    image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    png_image_write_to_memory(&image, nullptr, &size, 0, pixels, 0, nullptr);
    std::vector<std::uint8_t> output(size);
    if (png_image_write_to_memory(
            &image, output.data(), &size, 0, pixels, 0, nullptr)
        == 0) {
        return {};
    }
    output.resize(size);
    return output;
}

std::shared_ptr<const scene::ImageResource> image_resource(
    std::uint64_t id,
    const std::vector<std::uint8_t>& encoded)
{
    auto result = resource::decode_image(
        resource::EncodedView { encoded.data(), encoded.size() });
    auto* decoded = std::get_if<resource::DecodedImage>(&result);
    if (decoded == nullptr) {
        return nullptr;
    }
    return std::make_shared<const scene::ImageResource>(scene::ImageResource {
        id, encoded, std::move(*decoded) });
}

broker::OutboundPacket descriptor_resource_packet(
    std::uint64_t revision,
    const std::vector<std::uint8_t>& encoded)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto resource = MangoOverlay::Wire::CreateResourceAvailable(
        builder,
        revision,
        builder.CreateString("resource.mirror.test"),
        builder.CreateString("primary"),
        9,
        static_cast<std::uint32_t>(encoded.size()),
        {});
    builder.Finish(resource);
    return {
        protocol::encode_packet(
            protocol::PacketHeader {
                protocol::ProtocolVersion { 1, 0 },
                protocol::MessageType::resource_available,
                protocol::packet_flag_file_descriptor,
                90,
            },
            protocol::ByteView { builder.GetBufferPointer(), builder.GetSize() }),
        nullptr,
    };
}

renderer::ApplyResult apply(
    renderer::SceneMirror& mirror,
    const broker::OutboundPacket& packet,
    int descriptor = -1)
{
    return mirror.apply_packet(
        protocol::ByteView { packet.bytes.data(), packet.bytes.size() }, descriptor);
}

} // namespace

int main()
{
    const auto png = make_png();
    const auto resource = image_resource(9, png);
    scene::SceneStore store;
    store.register_provider(
        1,
        scene::ProviderIdentity {
            "resource.mirror.test",
            "primary",
            "Resource Mirror",
            1280,
            800,
            scene::Visibility::always,
        });
    if (resource == nullptr
        || store.store_resource(1, resource) != scene::ResourceResult::stored
        || store.commit(
               1,
               scene::SceneTransaction {
                   1,
                   { scene::UpsertElement { scene::Element {
                       1,
                       0,
                       scene::ImageElement {
                           { 20.0F, 30.0F },
                           { 100.0F, 50.0F },
                           9,
                           { 1.0F, 1.0F, 1.0F, 1.0F },
                       },
                   } } },
               })
            != scene::CommitResult::applied) {
        std::fputs("could not prepare a resource scene\n", stderr);
        return 1;
    }

    const auto packets = broker::encode_snapshot_packets(
        *store.snapshot(), protocol::ProtocolVersion { 1, 0 }, 90);
    renderer::SceneMirror inline_mirror(protocol::ProtocolVersion { 1, 0 });
    if (packets.size() != 4
        || apply(inline_mirror, packets[0]) != renderer::ApplyResult::accepted
        || apply(inline_mirror, packets[1]) != renderer::ApplyResult::accepted
        || apply(inline_mirror, packets[2]) != renderer::ApplyResult::accepted
        || inline_mirror.snapshot()->revision != 0
        || apply(inline_mirror, packets[3]) != renderer::ApplyResult::published
        || inline_mirror.snapshot()->providers[0]->resources.size() != 1
        || inline_mirror.snapshot()->providers[0]->resources[0]->decoded.width != 2) {
        std::fputs("inline resource snapshot was not published atomically\n", stderr);
        return 1;
    }

    const auto descriptor_packet = descriptor_resource_packet(
        store.snapshot()->revision, png);
    auto descriptor = protocol::make_sealed_memfd(
        protocol::ByteView { png.data(), png.size() });
    renderer::SceneMirror descriptor_mirror(protocol::ProtocolVersion { 1, 0 });
    if (!descriptor
        || apply(descriptor_mirror, packets[0]) != renderer::ApplyResult::accepted
        || apply(descriptor_mirror, descriptor_packet, descriptor.get())
            != renderer::ApplyResult::accepted
        || apply(descriptor_mirror, packets[2]) != renderer::ApplyResult::accepted
        || apply(descriptor_mirror, packets[3]) != renderer::ApplyResult::published) {
        std::fputs("descriptor resource snapshot was rejected\n", stderr);
        return 1;
    }

    const std::vector<std::uint8_t> damaged { 1, 2, 3, 4 };
    auto damaged_packet = descriptor_resource_packet(
        store.snapshot()->revision, damaged);
    auto damaged_descriptor = protocol::make_sealed_memfd(
        protocol::ByteView { damaged.data(), damaged.size() });
    renderer::SceneMirror damaged_mirror(protocol::ProtocolVersion { 1, 0 });
    if (!damaged_descriptor
        || apply(damaged_mirror, packets[0]) != renderer::ApplyResult::accepted
        || apply(damaged_mirror, damaged_packet, damaged_descriptor.get())
            != renderer::ApplyResult::invalid_payload
        || damaged_mirror.snapshot()->revision != 0) {
        std::fputs("damaged resource changed the visible snapshot\n", stderr);
        return 1;
    }

    scene::SceneLimits tight_limits;
    tight_limits.maximum_decoded_resource_bytes_global = 7;
    renderer::SceneMirror bounded_mirror(
        protocol::ProtocolVersion { 1, 0 }, tight_limits);
    if (apply(bounded_mirror, packets[0]) != renderer::ApplyResult::accepted
        || apply(bounded_mirror, packets[1])
            != renderer::ApplyResult::invalid_payload
        || bounded_mirror.snapshot()->revision != 0) {
        std::fputs("renderer global resource budget was not enforced\n", stderr);
        return 1;
    }

    return 0;
}
