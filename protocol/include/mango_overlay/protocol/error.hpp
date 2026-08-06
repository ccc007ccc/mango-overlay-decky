#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mango_overlay::protocol {

enum class ErrorCode : std::uint16_t {
    malformed_packet = 1,
    unexpected_message = 2,
    malformed_payload = 3,
    unsupported_version = 4,
    unsupported_required_capability = 5,
    invalid_role = 6,
    operation_failed = 7,
    not_found = 8,
};

struct ProtocolError {
    ErrorCode code;
    std::string message;
};

std::vector<std::uint8_t> encode_error(const ProtocolError& error);

} // namespace mango_overlay::protocol
