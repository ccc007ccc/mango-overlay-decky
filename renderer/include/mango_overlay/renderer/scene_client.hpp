#pragma once

#include "mango_overlay/scene/store.hpp"

#include <functional>
#include <memory>
#include <string>

namespace mango_overlay::renderer {

std::string default_socket_path();

class SceneClient {
public:
    explicit SceneClient(
        std::string socket_path = default_socket_path(),
        std::function<void()> on_scene_changed = {});
    ~SceneClient();

    SceneClient(const SceneClient&) = delete;
    SceneClient& operator=(const SceneClient&) = delete;

    void start();
    void stop();
    bool connected() const;
    std::shared_ptr<const scene::SceneSnapshot> snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mango_overlay::renderer
