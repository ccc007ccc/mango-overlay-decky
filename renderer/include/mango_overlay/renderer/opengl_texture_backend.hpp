#pragma once

#include "mango_overlay/renderer/texture_cache.hpp"

#include <memory>

namespace mango_overlay::renderer {

std::unique_ptr<TextureBackend> make_opengl_texture_backend();

} // namespace mango_overlay::renderer
