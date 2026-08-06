#pragma once

#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>

#include <memory>
#include <variant>

namespace mango_overlay::wire {

enum class SceneDecodeError {
    malformed_payload,
    invalid_mutation,
    invalid_element,
};

using ElementDecodeResult = std::variant<scene::Element, SceneDecodeError>;
using TransactionDecodeResult = std::variant<scene::SceneTransaction, SceneDecodeError>;

ElementDecodeResult decode_element(const MangoOverlay::Wire::Element& wire);

std::shared_ptr<const scene::ProviderScene> decode_provider(
    const MangoOverlay::Wire::ProviderScene* wire,
    const scene::SceneLimits& limits = {});

TransactionDecodeResult decode_transaction(protocol::ByteView payload);

flatbuffers::Offset<MangoOverlay::Wire::Element> encode_element(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::Element& element);

flatbuffers::Offset<MangoOverlay::Wire::Mutation> encode_mutation(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::SceneMutation& mutation);

flatbuffers::Offset<MangoOverlay::Wire::ProviderScene> encode_provider(
    flatbuffers::FlatBufferBuilder& builder,
    const scene::ProviderScene& provider);

} // namespace mango_overlay::wire
