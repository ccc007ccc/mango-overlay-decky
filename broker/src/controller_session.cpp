#include "mango_overlay/broker/controller_session.hpp"
#include "mango_overlay/protocol/controller.hpp"

#include <algorithm>
#include <variant>

namespace mango_overlay::broker {

ControllerSession::ControllerSession(
    scene::SceneStore& scenes,
    protocol::ProtocolVersion version,
    scene::PolicyCommit persist_policy)
    : scenes_(scenes)
    , version_(version)
    , persist_policy_(std::move(persist_policy))
{
}

ControllerSessionResponse ControllerSession::reject(
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

ControllerSessionResponse ControllerSession::status(std::uint64_t request_id)
{
    const auto policy = scenes_.policy();
    protocol::ControllerStatus response {
        policy.enabled,
        policy.require_approval,
        scenes_.snapshot()->revision,
        {},
    };
    response.applications.reserve(policy.applications.size());
    for (const auto& application : policy.applications) {
        response.applications.push_back(protocol::ControllerApplicationStatus {
            application.application_id,
            application.display_name,
            application.approved,
            application.visible,
            application.order,
            application.active_instances,
        });
    }
    const auto payload = protocol::encode_controller_status(response);
    return {
        protocol::encode_packet(
            protocol::PacketHeader {
                version_,
                protocol::MessageType::controller_status,
                0,
                request_id,
            },
            protocol::ByteView { payload.data(), payload.size() }),
        false,
    };
}

ControllerSessionResponse ControllerSession::process(protocol::ByteView packet_bytes)
{
    const auto decoded = protocol::decode_packet(packet_bytes);
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded);
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
    if (packet->header.flags != 0) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::malformed_packet,
            "The packet contains unsupported flags");
    }

    const auto request = protocol::decode_controller_request(
        packet->header.message_type, packet->payload);
    if (const auto* error = std::get_if<protocol::ControllerDecodeError>(&request)) {
        return reject(
            packet->header.request_id,
            *error == protocol::ControllerDecodeError::unexpected_message
                ? protocol::ErrorCode::unexpected_message
                : protocol::ErrorCode::malformed_payload,
            "The controller request is invalid");
    }

    bool applied = true;
    if (const auto* enabled = std::get_if<protocol::ControllerSetEnabled>(&request)) {
        applied = scenes_.set_enabled(enabled->enabled, persist_policy_);
    } else if (const auto* approval
        = std::get_if<protocol::ControllerSetRequireApproval>(&request)) {
        applied = scenes_.set_require_approval(approval->required, persist_policy_);
    } else if (const auto* application
        = std::get_if<protocol::ControllerSetApplicationPolicy>(&request)) {
        const auto current = scenes_.policy();
        const bool exists = std::any_of(
            current.applications.begin(),
            current.applications.end(),
            [&](const scene::ApplicationPolicy& candidate) {
                return candidate.application_id == application->application_id;
            });
        if (!exists) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::not_found,
                "The provider application is unknown");
        }
        applied = scenes_.set_application_policy(
            application->application_id,
            scene::ApplicationPolicyUpdate {
                application->approved,
                application->visible,
                application->order,
            },
            persist_policy_);
    } else if (const auto* position
        = std::get_if<protocol::ControllerSetApplicationPosition>(&request)) {
        const auto current = scenes_.policy();
        const bool exists = std::any_of(
            current.applications.begin(),
            current.applications.end(),
            [&](const scene::ApplicationPolicy& candidate) {
                return candidate.application_id == position->application_id;
            });
        if (!exists) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::not_found,
                "The provider application is unknown");
        }
        if (position->position >= current.applications.size()) {
            return reject(
                packet->header.request_id,
                protocol::ErrorCode::malformed_payload,
                "The provider position is out of range");
        }
        applied = scenes_.set_application_position(
            position->application_id, position->position, persist_policy_);
    }
    if (!applied) {
        return reject(
            packet->header.request_id,
            protocol::ErrorCode::operation_failed,
            "The policy could not be saved");
    }
    return status(packet->header.request_id);
}

} // namespace mango_overlay::broker
