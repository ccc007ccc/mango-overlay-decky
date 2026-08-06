#pragma once

#include "mango_overlay/scene/store.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mango_overlay::renderer {

using TextureHandle = std::uint64_t;

class TextureBackend {
public:
    virtual ~TextureBackend() = default;
    virtual TextureHandle upload_rgba(
        std::uint32_t width,
        std::uint32_t height,
        const std::uint8_t* rgba,
        std::size_t size) = 0;
    virtual void destroy(TextureHandle handle) = 0;
};

class TextureCache {
public:
    TextureCache();
    ~TextureCache();

    TextureCache(TextureCache&&) noexcept;
    TextureCache& operator=(TextureCache&&) noexcept;
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    void synchronize(
        const scene::SceneSnapshot& snapshot,
        TextureBackend& backend,
        std::size_t maximum_uploads = 1);
    TextureHandle frame(
        const scene::ProviderScene& provider,
        scene::ResourceId resource_id,
        std::size_t frame_index) const;
    void clear(TextureBackend& backend);
    void invalidate() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::size_t gif_frame_at(
    const resource::DecodedImage& image,
    std::uint64_t elapsed_ms,
    float playback_rate,
    bool paused,
    std::uint32_t first_frame);

} // namespace mango_overlay::renderer
