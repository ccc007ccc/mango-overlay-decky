#include "mango_overlay/protocol/controller.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include <algorithm>

namespace mango_overlay::protocol {

namespace {

constexpr std::size_t maximum_controller_applications = 256;

bool valid_string(const std::string& value)
{
    return !value.empty() && value.size() <= 128
        && std::find(value.begin(), value.end(), '\0') == value.end();
}

template <typename Message>
const Message* verified(ByteView payload)
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

template <typename Offset>
std::vector<std::uint8_t> finish(
    flatbuffers::FlatBufferBuilder& builder,
    Offset offset)
{
    builder.Finish(offset);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

} // namespace

std::vector<std::uint8_t> encode_controller_request(ControllerGetStatus)
{
    flatbuffers::FlatBufferBuilder builder;
    return finish(builder, MangoOverlay::Wire::CreateControllerGetStatus(builder));
}

std::vector<std::uint8_t> encode_controller_request(ControllerSetEnabled request)
{
    flatbuffers::FlatBufferBuilder builder;
    return finish(
        builder,
        MangoOverlay::Wire::CreateControllerSetEnabled(builder, request.enabled));
}

std::vector<std::uint8_t> encode_controller_request(
    ControllerSetRequireApproval request)
{
    flatbuffers::FlatBufferBuilder builder;
    return finish(
        builder,
        MangoOverlay::Wire::CreateControllerSetRequireApproval(
            builder, request.required));
}

std::vector<std::uint8_t> encode_controller_request(
    const ControllerSetApplicationPolicy& request)
{
    flatbuffers::FlatBufferBuilder builder;
    return finish(
        builder,
        MangoOverlay::Wire::CreateControllerSetApplicationPolicy(
            builder,
            builder.CreateString(request.application_id),
            request.approved,
            request.visible,
            request.order));
}

std::vector<std::uint8_t> encode_controller_request(
    const ControllerSetApplicationPosition& request)
{
    flatbuffers::FlatBufferBuilder builder;
    return finish(
        builder,
        MangoOverlay::Wire::CreateControllerSetApplicationPosition(
            builder,
            builder.CreateString(request.application_id),
            request.position));
}

ControllerRequest decode_controller_request(MessageType type, ByteView payload)
{
    switch (type) {
    case MessageType::controller_get_status:
        if (verified<MangoOverlay::Wire::ControllerGetStatus>(payload) != nullptr) {
            return ControllerGetStatus {};
        }
        break;
    case MessageType::controller_set_enabled: {
        const auto* message
            = verified<MangoOverlay::Wire::ControllerSetEnabled>(payload);
        if (message != nullptr) {
            return ControllerSetEnabled { message->enabled() };
        }
        break;
    }
    case MessageType::controller_set_require_approval: {
        const auto* message
            = verified<MangoOverlay::Wire::ControllerSetRequireApproval>(payload);
        if (message != nullptr) {
            return ControllerSetRequireApproval { message->required() };
        }
        break;
    }
    case MessageType::controller_set_application_policy: {
        const auto* message
            = verified<MangoOverlay::Wire::ControllerSetApplicationPolicy>(payload);
        if (message == nullptr || message->application_id() == nullptr) {
            break;
        }
        const auto application_id = message->application_id()->str();
        if (!valid_string(application_id)) {
            return ControllerDecodeError::invalid_value;
        }
        return ControllerSetApplicationPolicy {
            application_id,
            message->approved(),
            message->visible(),
            message->order(),
        };
    }
    case MessageType::controller_set_application_position: {
        const auto* message
            = verified<MangoOverlay::Wire::ControllerSetApplicationPosition>(payload);
        if (message == nullptr || message->application_id() == nullptr) {
            break;
        }
        const auto application_id = message->application_id()->str();
        if (!valid_string(application_id)) {
            return ControllerDecodeError::invalid_value;
        }
        return ControllerSetApplicationPosition {
            application_id,
            message->position(),
        };
    }
    default:
        return ControllerDecodeError::unexpected_message;
    }
    return ControllerDecodeError::malformed_payload;
}

std::vector<std::uint8_t> encode_controller_status(const ControllerStatus& status)
{
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<MangoOverlay::Wire::ControllerApplicationStatus>>
        applications;
    applications.reserve(status.applications.size());
    for (const auto& application : status.applications) {
        applications.push_back(
            MangoOverlay::Wire::CreateControllerApplicationStatus(
                builder,
                builder.CreateString(application.application_id),
                builder.CreateString(application.display_name),
                application.approved,
                application.visible,
                application.order,
                application.active_instances));
    }
    return finish(
        builder,
        MangoOverlay::Wire::CreateControllerStatus(
            builder,
            status.enabled,
            status.require_approval,
            status.scene_revision,
            builder.CreateVector(applications)));
}

ControllerStatusDecodeResult decode_controller_status(ByteView payload)
{
    const auto* message = verified<MangoOverlay::Wire::ControllerStatus>(payload);
    if (message == nullptr || message->applications() == nullptr
        || message->applications()->size() > maximum_controller_applications) {
        return ControllerDecodeError::malformed_payload;
    }

    ControllerStatus result {
        message->enabled(),
        message->require_approval(),
        message->scene_revision(),
        {},
    };
    result.applications.reserve(message->applications()->size());
    for (const auto* application : *message->applications()) {
        if (application == nullptr || application->application_id() == nullptr
            || application->display_name() == nullptr) {
            return ControllerDecodeError::malformed_payload;
        }
        const auto application_id = application->application_id()->str();
        const auto display_name = application->display_name()->str();
        if (!valid_string(application_id) || !valid_string(display_name)) {
            return ControllerDecodeError::invalid_value;
        }
        result.applications.push_back(ControllerApplicationStatus {
            application_id,
            display_name,
            application->approved(),
            application->visible(),
            application->order(),
            application->active_instances(),
        });
    }
    return result;
}

} // namespace mango_overlay::protocol
