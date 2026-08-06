#pragma once

#include "mango_overlay/wire/scene_codec.hpp"

namespace mango_overlay::broker {

using SceneDecodeError = wire::SceneDecodeError;
using SceneDecodeResult = wire::TransactionDecodeResult;

SceneDecodeResult decode_scene_transaction(protocol::ByteView payload);

} // namespace mango_overlay::broker
