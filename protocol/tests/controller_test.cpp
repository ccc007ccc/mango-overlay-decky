#include "mango_overlay/protocol/controller.hpp"

#include <cstdio>
#include <variant>

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::ControllerApplicationStatus;
using mango_overlay::protocol::ControllerGetStatus;
using mango_overlay::protocol::ControllerSetApplicationPolicy;
using mango_overlay::protocol::ControllerSetApplicationPosition;
using mango_overlay::protocol::ControllerSetEnabled;
using mango_overlay::protocol::ControllerSetRequireApproval;
using mango_overlay::protocol::ControllerStatus;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::decode_controller_request;
using mango_overlay::protocol::decode_controller_status;
using mango_overlay::protocol::encode_controller_request;
using mango_overlay::protocol::encode_controller_status;

template <typename Request>
bool round_trips(MessageType type, const Request& request)
{
    const auto payload = encode_controller_request(request);
    const auto decoded = decode_controller_request(
        type, ByteView { payload.data(), payload.size() });
    return std::holds_alternative<Request>(decoded);
}

int main()
{
    if (!round_trips(MessageType::controller_get_status, ControllerGetStatus {})
        || !round_trips(
            MessageType::controller_set_enabled,
            ControllerSetEnabled { false })
        || !round_trips(
            MessageType::controller_set_require_approval,
            ControllerSetRequireApproval { true })
        || !round_trips(
            MessageType::controller_set_application_policy,
            ControllerSetApplicationPolicy {
                "dev.example.telemetry", false, true, -3 })
        || !round_trips(
            MessageType::controller_set_application_position,
            ControllerSetApplicationPosition { "dev.example.telemetry", 2 })) {
        std::fputs("controller request did not round-trip\n", stderr);
        return 1;
    }

    const auto malformed = decode_controller_request(
        MessageType::controller_set_enabled,
        ByteView { nullptr, 0 });
    if (!std::holds_alternative<mango_overlay::protocol::ControllerDecodeError>(
            malformed)) {
        std::fputs("malformed controller request was accepted\n", stderr);
        return 1;
    }

    const ControllerStatus status {
        false,
        true,
        42,
        {
            ControllerApplicationStatus {
                "dev.example.telemetry", "Telemetry", true, false, 7, 2 },
        },
    };
    const auto encoded = encode_controller_status(status);
    const auto decoded = decode_controller_status(
        ByteView { encoded.data(), encoded.size() });
    const auto* result = std::get_if<ControllerStatus>(&decoded);
    if (result == nullptr || result->enabled || !result->require_approval
        || result->scene_revision != 42 || result->applications.size() != 1
        || result->applications[0].application_id != "dev.example.telemetry"
        || result->applications[0].display_name != "Telemetry"
        || !result->applications[0].approved
        || result->applications[0].visible
        || result->applications[0].order != 7
        || result->applications[0].active_instances != 2) {
        std::fputs("controller status did not round-trip\n", stderr);
        return 1;
    }
    return 0;
}
