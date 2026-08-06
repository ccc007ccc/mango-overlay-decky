#include "mango_overlay/broker/scene_decoder.hpp"

namespace mango_overlay::broker {

SceneDecodeResult decode_scene_transaction(protocol::ByteView payload)
{
    return wire::decode_transaction(payload);
}

} // namespace mango_overlay::broker
