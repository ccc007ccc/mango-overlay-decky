#pragma once

#include <cstdint>
#include <vector>

namespace mango_overlay::protocol {

std::vector<std::uint8_t> encode_transaction_committed(
    std::uint64_t transaction_id,
    std::uint64_t scene_revision,
    bool already_applied);

} // namespace mango_overlay::protocol
