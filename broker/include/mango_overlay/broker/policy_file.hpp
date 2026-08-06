#pragma once

#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <cstdint>
#include <filesystem>
#include <variant>
#include <vector>

namespace mango_overlay::broker {

enum class PolicyFileError {
    none,
    unsafe_path,
    io_error,
    invalid_data,
};

using PolicyLoadResult = std::variant<scene::OverlayPolicy, PolicyFileError>;

PolicyLoadResult load_policy_file(const std::filesystem::path& path);
std::vector<std::uint8_t> encode_policy_file(const scene::OverlayPolicy& policy);
PolicyLoadResult decode_policy_file(protocol::ByteView contents);
PolicyFileError save_policy_file(
    const std::filesystem::path& path,
    const scene::OverlayPolicy& policy);

} // namespace mango_overlay::broker
