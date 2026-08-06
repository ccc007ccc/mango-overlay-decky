#include "mango_overlay/broker/provider_session.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>
#include <png.h>

#include <cstdio>
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

std::vector<std::uint8_t> packet(
    protocol::MessageType type,
    std::uint16_t flags,
    std::uint64_t request_id,
    flatbuffers::FlatBufferBuilder& builder)
{
    return protocol::encode_packet(
        protocol::PacketHeader {
            protocol::ProtocolVersion { 1, 0 }, type, flags, request_id },
        protocol::ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

bool register_provider(broker::ProviderSession& session)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto message = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        builder.CreateString("resource.protocol.test"),
        builder.CreateString("primary"),
        builder.CreateString("Resource Protocol"),
        1280,
        800,
        MangoOverlay::Wire::Visibility::Always);
    builder.Finish(message);
    const auto bytes = packet(
        protocol::MessageType::register_provider, 0, 1, builder);
    const auto response = session.process(
        protocol::ByteView { bytes.data(), bytes.size() });
    return !response.close_after_send;
}

broker::SessionResponse upload(
    broker::ProviderSession& session,
    std::uint64_t resource_id,
    const std::vector<std::uint8_t>& bytes,
    bool attached,
    int descriptor = -1)
{
    flatbuffers::FlatBufferBuilder builder;
    flatbuffers::Offset<flatbuffers::Vector<std::uint8_t>> inline_data;
    if (!attached) {
        inline_data = builder.CreateVector(bytes);
    }
    const auto message = MangoOverlay::Wire::CreateUploadResource(
        builder,
        resource_id,
        static_cast<std::uint32_t>(bytes.size()),
        inline_data);
    builder.Finish(message);
    const auto request = packet(
        protocol::MessageType::upload_resource,
        attached ? protocol::packet_flag_file_descriptor : 0,
        resource_id,
        builder);
    return session.process(
        protocol::ByteView { request.data(), request.size() }, descriptor);
}

bool stored_response(
    const broker::SessionResponse& response,
    std::uint64_t resource_id)
{
    const auto decoded = protocol::decode_packet(protocol::ByteView {
        response.packet.data(), response.packet.size() });
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded);
    if (response.close_after_send || packet == nullptr
        || packet->header.message_type != protocol::MessageType::resource_stored) {
        return false;
    }
    flatbuffers::Verifier verifier(packet->payload.data, packet->payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ResourceStored>(nullptr)) {
        return false;
    }
    const auto* stored
        = flatbuffers::GetRoot<MangoOverlay::Wire::ResourceStored>(packet->payload.data);
    return stored->resource_id() == resource_id && stored->width() == 2
        && stored->height() == 1 && stored->frame_count() == 1;
}

} // namespace

int main()
{
    scene::SceneStore scenes;
    broker::ProviderSession session(
        scenes, 44, protocol::ProtocolVersion { 1, 0 });
    const auto png = make_png();
    if (png.empty() || !register_provider(session)
        || !stored_response(upload(session, 10, png, false), 10)) {
        std::fputs("inline image upload failed\n", stderr);
        return 1;
    }

    auto descriptor = protocol::make_sealed_memfd(
        protocol::ByteView { png.data(), png.size() });
    if (!descriptor
        || !stored_response(
            upload(session, 11, png, true, descriptor.get()), 11)) {
        std::fputs("descriptor image upload failed\n", stderr);
        return 1;
    }

    const std::vector<std::uint8_t> damaged { 1, 2, 3, 4 };
    const auto rejected = upload(session, 12, damaged, false);
    const auto decoded = protocol::decode_packet(protocol::ByteView {
        rejected.packet.data(), rejected.packet.size() });
    const auto* error = std::get_if<protocol::DecodedPacketView>(&decoded);
    if (rejected.close_after_send || error == nullptr
        || error->header.message_type != protocol::MessageType::error
        || scenes.snapshot()->revision != 1) {
        std::fputs("damaged resource affected the provider scene\n", stderr);
        return 1;
    }

    return 0;
}
