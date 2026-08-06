#include "mango_overlay/renderer/texture_cache.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mango_overlay::renderer {

namespace {

std::string resource_key(
    const scene::ProviderScene& provider,
    scene::ResourceId resource_id)
{
    std::string key;
    key.reserve(
        provider.identity.application_id.size()
        + provider.identity.instance_id.size() + 2 + sizeof(resource_id));
    key.append(provider.identity.application_id);
    key.push_back('\0');
    key.append(provider.identity.instance_id);
    key.push_back('\0');
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        key.push_back(static_cast<char>(resource_id >> shift));
    }
    return key;
}

bool same_resource_content(
    const scene::ImageResource& left,
    const scene::ImageResource& right)
{
    return left.id == right.id && left.encoded == right.encoded;
}

} // namespace

struct TextureCache::Impl {
    struct Entry {
        std::shared_ptr<const scene::ImageResource> resource;
        std::vector<TextureHandle> handles;
    };

    std::unordered_map<std::string, Entry> entries;
};

TextureCache::TextureCache()
    : impl_(std::make_unique<Impl>())
{
}

TextureCache::~TextureCache() = default;
TextureCache::TextureCache(TextureCache&&) noexcept = default;
TextureCache& TextureCache::operator=(TextureCache&&) noexcept = default;

void TextureCache::synchronize(
    const scene::SceneSnapshot& snapshot,
    TextureBackend& backend,
    std::size_t maximum_uploads)
{
    std::unordered_set<std::string> retained;
    for (const auto& provider : snapshot.providers) {
        if (provider == nullptr) {
            continue;
        }
        for (const auto& resource : provider->resources) {
            if (resource == nullptr) {
                continue;
            }
            auto key = resource_key(*provider, resource->id);
            retained.insert(key);
            auto [entry, inserted] = impl_->entries.try_emplace(
                key,
                Impl::Entry {
                    resource,
                    std::vector<TextureHandle>(resource->decoded.frame_count(), 0),
                });
            if (!inserted && entry->second.resource != resource
                && !same_resource_content(*entry->second.resource, *resource)) {
                for (const auto handle : entry->second.handles) {
                    if (handle != 0) {
                        backend.destroy(handle);
                    }
                }
                entry->second = Impl::Entry {
                    resource,
                    std::vector<TextureHandle>(resource->decoded.frame_count(), 0),
                };
            }
        }
    }

    for (auto entry = impl_->entries.begin(); entry != impl_->entries.end();) {
        if (retained.find(entry->first) != retained.end()) {
            ++entry;
            continue;
        }
        for (const auto handle : entry->second.handles) {
            if (handle != 0) {
                backend.destroy(handle);
            }
        }
        entry = impl_->entries.erase(entry);
    }

    std::size_t uploaded = 0;
    for (auto& entry : impl_->entries) {
        const auto frame_stride = entry.second.resource->decoded.frame_stride();
        for (std::size_t index = 0; index < entry.second.handles.size(); ++index) {
            if (uploaded >= maximum_uploads) {
                return;
            }
            if (entry.second.handles[index] != 0) {
                continue;
            }
            const auto offset = index * frame_stride;
            const auto handle = backend.upload_rgba(
                entry.second.resource->decoded.width,
                entry.second.resource->decoded.height,
                entry.second.resource->decoded.rgba.data() + offset,
                frame_stride);
            if (handle == 0) {
                return;
            }
            entry.second.handles[index] = handle;
            ++uploaded;
        }
    }
}

TextureHandle TextureCache::frame(
    const scene::ProviderScene& provider,
    scene::ResourceId resource_id,
    std::size_t frame_index) const
{
    const auto entry = impl_->entries.find(resource_key(provider, resource_id));
    if (entry == impl_->entries.end() || entry->second.handles.empty()) {
        return 0;
    }
    if (frame_index < entry->second.handles.size()
        && entry->second.handles[frame_index] != 0) {
        return entry->second.handles[frame_index];
    }
    const auto available = std::find_if(
        entry->second.handles.begin(),
        entry->second.handles.end(),
        [](TextureHandle handle) { return handle != 0; });
    return available == entry->second.handles.end() ? 0 : *available;
}

void TextureCache::clear(TextureBackend& backend)
{
    for (const auto& entry : impl_->entries) {
        for (const auto handle : entry.second.handles) {
            if (handle != 0) {
                backend.destroy(handle);
            }
        }
    }
    impl_->entries.clear();
}

void TextureCache::invalidate() noexcept
{
    for (auto& entry : impl_->entries) {
        std::fill(entry.second.handles.begin(), entry.second.handles.end(), 0);
    }
}

std::size_t gif_frame_at(
    const resource::DecodedImage& image,
    std::uint64_t elapsed_ms,
    float playback_rate,
    bool paused,
    std::uint32_t first_frame)
{
    const auto frame_count = image.frame_count();
    if (frame_count == 0) {
        return 0;
    }
    std::size_t frame = first_frame % frame_count;
    if (paused || frame_count == 1 || !std::isfinite(playback_rate)
        || playback_rate <= 0.0F) {
        return frame;
    }

    std::uint64_t cycle_ms = 0;
    for (const auto duration : image.frame_durations_ms) {
        cycle_ms += duration;
    }
    if (cycle_ms == 0) {
        return frame;
    }
    double position = std::fmod(
        static_cast<double>(elapsed_ms) * static_cast<double>(playback_rate),
        static_cast<double>(cycle_ms));
    for (std::size_t offset = 0; offset < frame_count; ++offset) {
        const auto candidate = (frame + offset) % frame_count;
        const auto duration = image.frame_durations_ms[candidate];
        if (position < duration) {
            return candidate;
        }
        position -= duration;
    }
    return frame;
}

} // namespace mango_overlay::renderer
