#include "mango_overlay/renderer/texture_cache.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace mango_overlay;

namespace {

struct FakeBackend final : renderer::TextureBackend {
    renderer::TextureHandle upload_rgba(
        std::uint32_t,
        std::uint32_t,
        const std::uint8_t*,
        std::size_t size) override
    {
        uploaded_sizes.push_back(size);
        return next_handle++;
    }

    void destroy(renderer::TextureHandle handle) override
    {
        destroyed.push_back(handle);
    }

    renderer::TextureHandle next_handle = 1;
    std::vector<std::size_t> uploaded_sizes;
    std::vector<renderer::TextureHandle> destroyed;
};

std::shared_ptr<const scene::ImageResource> make_resource(
    std::uint64_t id,
    std::uint8_t content_revision = 0)
{
    return std::make_shared<const scene::ImageResource>(scene::ImageResource {
        id,
        { 1, 2, 3, content_revision },
        resource::DecodedImage {
            resource::ImageFormat::gif,
            1,
            1,
            { 20, 30 },
            {
                static_cast<std::uint8_t>(255 - content_revision), 0, 0, 255,
                0, 255, 0, 255,
            },
        },
    });
}

scene::SceneSnapshot snapshot(
    const std::shared_ptr<const scene::ImageResource>& image)
{
    auto provider = std::make_shared<scene::ProviderScene>(scene::ProviderScene {
        scene::ProviderIdentity {
            "texture.test", "primary", "Texture Test", 1280, 800,
            scene::Visibility::always },
        {},
        { image },
    });
    return { 1, { std::move(provider) } };
}

} // namespace

int main()
{
    renderer::TextureCache cache;
    FakeBackend backend;
    const auto first_resource = make_resource(10);
    auto first = snapshot(first_resource);
    cache.synchronize(first, backend, 1);
    if (backend.uploaded_sizes.size() != 1
        || cache.frame(*first.providers[0], 10, 0) == 0
        || cache.frame(*first.providers[0], 10, 1) == 0) {
        std::fputs("texture upload budget or fallback handle is incorrect\n", stderr);
        return 1;
    }
    cache.synchronize(first, backend, 1);
    if (backend.uploaded_sizes.size() != 2
        || cache.frame(*first.providers[0], 10, 1) == 0) {
        std::fputs("animated frames were not uploaded incrementally\n", stderr);
        return 1;
    }

    cache.invalidate();
    cache.synchronize(first, backend, 2);
    if (backend.uploaded_sizes.size() != 4 || !backend.destroyed.empty()) {
        std::fputs("context invalidation did not rebuild texture handles\n", stderr);
        return 1;
    }

    const auto equivalent_resource = make_resource(10);
    auto equivalent = snapshot(equivalent_resource);
    cache.synchronize(equivalent, backend, 1);
    if (backend.destroyed.size() != 0 || backend.uploaded_sizes.size() != 4) {
        std::fputs("equivalent retransmitted resource rebuilt textures\n", stderr);
        return 1;
    }

    const auto changed_resource = make_resource(10, 1);
    auto changed = snapshot(changed_resource);
    cache.synchronize(changed, backend, 1);
    if (backend.destroyed.size() != 2 || backend.uploaded_sizes.size() != 5) {
        std::fputs("changed resource retained stale texture handles\n", stderr);
        return 1;
    }

    const scene::SceneSnapshot empty { 2, {} };
    cache.synchronize(empty, backend, 1);
    if (backend.destroyed.size() != 3
        || cache.frame(*changed.providers[0], 10, 0) != 0) {
        std::fputs("unused textures were not released\n", stderr);
        return 1;
    }

    const auto& animation = first_resource->decoded;
    if (renderer::gif_frame_at(animation, 0, 1.0F, false, 0) != 0
        || renderer::gif_frame_at(animation, 19, 1.0F, false, 0) != 0
        || renderer::gif_frame_at(animation, 20, 1.0F, false, 0) != 1
        || renderer::gif_frame_at(animation, 49, 1.0F, false, 0) != 1
        || renderer::gif_frame_at(animation, 50, 1.0F, false, 0) != 0
        || renderer::gif_frame_at(animation, 0, 1.0F, true, 1) != 1) {
        std::fputs("GIF timing did not honor durations or paused frame\n", stderr);
        return 1;
    }
    return 0;
}
