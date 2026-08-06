#include "mango_overlay/renderer/opengl_texture_backend.hpp"

#include "mango_overlay/renderer/texture_cache.hpp"

#include <GL/gl.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace mango_overlay::renderer {

namespace {

class OpenGlTextureBackend final : public TextureBackend {
public:
    TextureHandle upload_rgba(
        std::uint32_t width,
        std::uint32_t height,
        const std::uint8_t* rgba,
        std::size_t size) override
    {
        if (width == 0 || height == 0 || rgba == nullptr
            || width > std::numeric_limits<std::size_t>::max() / height / 4
            || size != static_cast<std::size_t>(width) * height * 4) {
            return 0;
        }

        GLint previous_texture = 0;
        GLint previous_unpack_alignment = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        if (texture == 0) {
            return 0;
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            rgba);

        glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
        return texture;
    }

    void destroy(TextureHandle handle) override
    {
        const auto texture = static_cast<GLuint>(handle);
        glDeleteTextures(1, &texture);
    }
};

} // namespace

std::unique_ptr<TextureBackend> make_opengl_texture_backend()
{
    return std::make_unique<OpenGlTextureBackend>();
}

} // namespace mango_overlay::renderer
