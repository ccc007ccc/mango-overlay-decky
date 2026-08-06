#include "mango_overlay/protocol/transaction.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>

namespace mango_overlay::protocol {

std::vector<std::uint8_t> encode_transaction_committed(
    std::uint64_t transaction_id,
    std::uint64_t scene_revision,
    bool already_applied)
{
    flatbuffers::FlatBufferBuilder builder;
    const auto committed = MangoOverlay::Wire::CreateTransactionCommitted(
        builder, transaction_id, scene_revision, already_applied);
    builder.Finish(committed);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

} // namespace mango_overlay::protocol
