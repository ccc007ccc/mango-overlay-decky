#pragma once

#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/scene/store.hpp"

#include <memory>

namespace mango_overlay::renderer {

enum class ApplyResult {
    accepted,
    published,
    revision_gap,
    invalid_packet,
    unexpected_message,
    invalid_payload,
};

class SceneMirror {
public:
    explicit SceneMirror(
        protocol::ProtocolVersion version,
        scene::SceneLimits limits = {});
    ~SceneMirror();

    SceneMirror(SceneMirror&&) noexcept;
    SceneMirror& operator=(SceneMirror&&) noexcept;
    SceneMirror(const SceneMirror&) = delete;
    SceneMirror& operator=(const SceneMirror&) = delete;

    ApplyResult apply_packet(protocol::ByteView packet, int attachment_fd = -1);
    std::shared_ptr<const scene::SceneSnapshot> snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mango_overlay::renderer
