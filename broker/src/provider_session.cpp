#include "mango_overlay/broker/provider_session.hpp"
#include "mango_overlay/broker/scene_decoder.hpp"
#include "mango_overlay/protocol/registration.hpp"
#include "mango_overlay/protocol/transaction.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay/resource/image.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>

#include <memory>
#include <variant>

namespace mango_overlay::broker {

namespace {

scene::Visibility map_visibility(protocol::Visibility visibility)
{
    switch (visibility) {
    case protocol::Visibility::game_only:
        return scene::Visibility::game_only;
    case protocol::Visibility::steam_only:
        return scene::Visibility::steam_only;
    case protocol::Visibility::always:
        return scene::Visibility::always;
    }
    return scene::Visibility::game_only;
}

template <typename Message>
const Message* verified_message(protocol::ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<Message>(nullptr)) {
        return nullptr;
    }
    return flatbuffers::GetRoot<Message>(payload.data);
}

std::vector<std::uint8_t> response_packet(
    protocol::ProtocolVersion version,
    protocol::MessageType type,
    std::uint64_t request_id,
    flatbuffers::FlatBufferBuilder& builder)
{
    return protocol::encode_packet(
        protocol::PacketHeader { version, type, 0, request_id },
        protocol::ByteView { builder.GetBufferPointer(), builder.GetSize() });
}

} // namespace

ProviderSession::ProviderSession(
    scene::SceneStore& scenes,
    scene::ConnectionId connection,
    protocol::ProtocolVersion version)
    : scenes_(scenes)
    , connection_(connection)
    , version_(version)
{
}

ProviderSession::~ProviderSession()
{
    scenes_.disconnect(connection_);
}

SessionResponse ProviderSession::reject(
    std::uint64_t request_id,
    protocol::ErrorCode code,
    const char* message)
{
    constexpr std::uint8_t maximum_protocol_errors = 3;
    const auto payload = protocol::encode_error(protocol::ProtocolError { code, message });
    ++protocol_errors_;
    return {
        protocol::encode_packet(
            protocol::PacketHeader {
                version_,
                protocol::MessageType::error,
                0,
                request_id,
            },
            protocol::ByteView { payload.data(), payload.size() }),
        protocol_errors_ >= maximum_protocol_errors,
    };
}

SessionResponse ProviderSession::process(protocol::ByteView packet_bytes, int attachment_fd)
{
    const auto decoded_packet = protocol::decode_packet(packet_bytes);
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded_packet);
    if (packet == nullptr) {
        return reject(
            0,
            protocol::ErrorCode::malformed_packet,
            "The packet header or size is invalid");
    }
    if (packet->header.version.major != version_.major
        || packet->header.version.minor != version_.minor) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::unsupported_version,
            "The packet version differs from the negotiated version");
    }
    const bool has_attachment = attachment_fd >= 0;
    const bool expects_attachment
        = (packet->header.flags & protocol::packet_flag_file_descriptor) != 0;
    if ((packet->header.flags & ~protocol::packet_flag_file_descriptor) != 0
        || has_attachment != expects_attachment
        || (expects_attachment
            && packet->header.message_type != protocol::MessageType::upload_resource)) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::malformed_packet,
            "The packet contains unsupported flags");
    }

    if (!registered_) {
        if (packet->header.message_type != protocol::MessageType::register_provider) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::unexpected_message,
                "A provider must register before sending scene messages");
        }
        const auto decoded_registration = protocol::decode_registration(packet->payload);
        const auto* registration = std::get_if<protocol::ProviderRegistration>(&decoded_registration);
        if (registration == nullptr) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The provider registration is invalid");
        }

        const auto result = scenes_.register_provider(
            connection_,
            scene::ProviderIdentity {
                registration->application_id,
                registration->instance_id,
                registration->display_name,
                registration->canvas_width,
                registration->canvas_height,
                map_visibility(registration->requested_visibility),
            });
        if (result != scene::RegistrationResult::registered) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The provider identity could not be registered");
        }
        registered_ = true;

        const auto response_payload = protocol::encode_provider_registered(
            scenes_.snapshot()->revision);
        return {
            protocol::encode_packet(
                protocol::PacketHeader {
                    version_,
                    protocol::MessageType::provider_registered,
                    0,
                    packet->header.request_id,
                },
                protocol::ByteView { response_payload.data(), response_payload.size() }),
            false,
        };
    }

    if (packet->header.message_type == protocol::MessageType::upload_resource) {
        const auto* upload
            = verified_message<MangoOverlay::Wire::UploadResource>(packet->payload);
        if (upload == nullptr || upload->resource_id() == 0
            || upload->encoded_size() == 0) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The image resource metadata is invalid");
        }

        const auto limits = scenes_.limits();
        std::vector<std::uint8_t> encoded;
        const auto* inline_data = upload->inline_data();
        if (expects_attachment) {
            if ((inline_data != nullptr && inline_data->size() != 0)
                || !protocol::read_resource_descriptor(
                    attachment_fd,
                    upload->encoded_size(),
                    limits.maximum_encoded_resource_bytes,
                    encoded)) {
                return reject(
                    packet->header.request_id,
                    protocol::ErrorCode::malformed_payload,
                    "The image resource descriptor is invalid");
            }
        } else {
            if (inline_data == nullptr
                || inline_data->size() != upload->encoded_size()
                || inline_data->size() > limits.maximum_encoded_resource_bytes) {
                return reject(
                    packet->header.request_id,
                    protocol::ErrorCode::malformed_payload,
                    "The inline image resource is invalid");
            }
            encoded.assign(inline_data->begin(), inline_data->end());
        }

        resource::ImageLimits image_limits;
        image_limits.maximum_encoded_bytes = limits.maximum_encoded_resource_bytes;
        image_limits.maximum_decoded_bytes = limits.maximum_decoded_resource_bytes;
        auto decoded = resource::decode_image(
            resource::EncodedView { encoded.data(), encoded.size() }, image_limits);
        auto* image = std::get_if<resource::DecodedImage>(&decoded);
        if (image == nullptr) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The image resource could not be decoded within the limits");
        }

        const auto width = image->width;
        const auto height = image->height;
        const auto frame_count = static_cast<std::uint32_t>(image->frame_count());
        auto stored = std::make_shared<const scene::ImageResource>(scene::ImageResource {
            upload->resource_id(), std::move(encoded), std::move(*image) });
        if (scenes_.store_resource(connection_, std::move(stored))
            != scene::ResourceResult::stored) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The image resource could not be stored");
        }

        flatbuffers::FlatBufferBuilder builder;
        const auto response = MangoOverlay::Wire::CreateResourceStored(
            builder, upload->resource_id(), width, height, frame_count);
        builder.Finish(response);
        return {
            response_packet(
                version_,
                protocol::MessageType::resource_stored,
                packet->header.request_id,
                builder),
            false,
        };
    }

    if (packet->header.message_type == protocol::MessageType::release_resource) {
        const auto* release
            = verified_message<MangoOverlay::Wire::ReleaseResource>(packet->payload);
        if (release == nullptr || release->resource_id() == 0
            || scenes_.release_resource(connection_, release->resource_id())
                != scene::ResourceResult::released) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The image resource could not be released");
        }
        flatbuffers::FlatBufferBuilder builder;
        const auto response = MangoOverlay::Wire::CreateResourceReleased(
            builder, release->resource_id());
        builder.Finish(response);
        return {
            response_packet(
                version_,
                protocol::MessageType::resource_released,
                packet->header.request_id,
                builder),
            false,
        };
    }

    if (packet->header.message_type != protocol::MessageType::scene_transaction) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::unexpected_message,
            "A registered provider may only manage resources or submit scene transactions");
    }
    const auto decoded_transaction = decode_scene_transaction(packet->payload);
    const auto* transaction = std::get_if<scene::SceneTransaction>(&decoded_transaction);
    if (transaction == nullptr) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::malformed_payload,
            "The scene transaction is invalid");
    }
    const auto commit_result = scenes_.commit(connection_, *transaction);
    const bool already_applied = commit_result == scene::CommitResult::already_applied;
    if (commit_result != scene::CommitResult::applied && !already_applied) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::malformed_payload,
            "The scene transaction could not be committed");
    }

    const auto response_payload = protocol::encode_transaction_committed(
        transaction->transaction_id,
        scenes_.snapshot()->revision,
        already_applied);
    return {
        protocol::encode_packet(
            protocol::PacketHeader {
                version_,
                protocol::MessageType::transaction_committed,
                0,
                packet->header.request_id,
            },
            protocol::ByteView { response_payload.data(), response_payload.size() }),
        false,
    };
}

} // namespace mango_overlay::broker
