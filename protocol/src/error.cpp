#include "mango_overlay/protocol/error.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>

#include <algorithm>
#include <stdexcept>

namespace mango_overlay::protocol {

namespace {

MangoOverlay::Wire::ErrorCode encode_error_code(ErrorCode code)
{
    switch (code) {
    case ErrorCode::malformed_packet:
        return MangoOverlay::Wire::ErrorCode::MalformedPacket;
    case ErrorCode::unexpected_message:
        return MangoOverlay::Wire::ErrorCode::UnexpectedMessage;
    case ErrorCode::malformed_payload:
        return MangoOverlay::Wire::ErrorCode::MalformedPayload;
    case ErrorCode::unsupported_version:
        return MangoOverlay::Wire::ErrorCode::UnsupportedVersion;
    case ErrorCode::unsupported_required_capability:
        return MangoOverlay::Wire::ErrorCode::UnsupportedRequiredCapability;
    case ErrorCode::invalid_role:
        return MangoOverlay::Wire::ErrorCode::InvalidRole;
    case ErrorCode::operation_failed:
        return MangoOverlay::Wire::ErrorCode::OperationFailed;
    case ErrorCode::not_found:
        return MangoOverlay::Wire::ErrorCode::NotFound;
    }
    throw std::invalid_argument("Mango Overlay error has an invalid code");
}

} // namespace

std::vector<std::uint8_t> encode_error(const ProtocolError& error)
{
    if (error.message.empty() || error.message.size() > 512
        || std::find(error.message.begin(), error.message.end(), '\0') != error.message.end()) {
        throw std::invalid_argument("Mango Overlay error has an invalid message");
    }

    flatbuffers::FlatBufferBuilder builder;
    const auto message = builder.CreateString(error.message);
    const auto response = MangoOverlay::Wire::CreateErrorResponse(
        builder, encode_error_code(error.code), message);
    builder.Finish(response);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

} // namespace mango_overlay::protocol
