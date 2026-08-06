#pragma once

#include "mango_overlay/protocol/packet.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace mango_overlay::protocol {

struct ControllerGetStatus {
};

struct ControllerSetEnabled {
    bool enabled;
};

struct ControllerSetRequireApproval {
    bool required;
};

struct ControllerSetApplicationPolicy {
    std::string application_id;
    bool approved;
    bool visible;
    std::int32_t order;
};

struct ControllerSetApplicationPosition {
    std::string application_id;
    std::uint32_t position;
};

struct ControllerApplicationStatus {
    std::string application_id;
    std::string display_name;
    bool approved;
    bool visible;
    std::int32_t order;
    std::uint32_t active_instances;
};

struct ControllerStatus {
    bool enabled;
    bool require_approval;
    std::uint64_t scene_revision;
    std::vector<ControllerApplicationStatus> applications;
};

enum class ControllerDecodeError {
    malformed_payload,
    invalid_value,
    unexpected_message,
};

using ControllerRequest = std::variant<
    ControllerGetStatus,
    ControllerSetEnabled,
    ControllerSetRequireApproval,
    ControllerSetApplicationPolicy,
    ControllerSetApplicationPosition,
    ControllerDecodeError>;

using ControllerStatusDecodeResult
    = std::variant<ControllerStatus, ControllerDecodeError>;

std::vector<std::uint8_t> encode_controller_request(ControllerGetStatus request);
std::vector<std::uint8_t> encode_controller_request(ControllerSetEnabled request);
std::vector<std::uint8_t> encode_controller_request(
    ControllerSetRequireApproval request);
std::vector<std::uint8_t> encode_controller_request(
    const ControllerSetApplicationPolicy& request);
std::vector<std::uint8_t> encode_controller_request(
    const ControllerSetApplicationPosition& request);
ControllerRequest decode_controller_request(MessageType type, ByteView payload);

std::vector<std::uint8_t> encode_controller_status(const ControllerStatus& status);
ControllerStatusDecodeResult decode_controller_status(ByteView payload);

} // namespace mango_overlay::protocol
