#include "mango_overlay/renderer/scene_mirror.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay/resource/image.hpp"
#include "mango_overlay/scene/validation.hpp"
#include "mango_overlay/wire/scene_codec.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace mango_overlay::renderer {

namespace {

using ProviderMap
    = std::unordered_map<std::string, std::shared_ptr<const scene::ProviderScene>>;
using ResourceMap
    = std::unordered_map<scene::ResourceId, std::shared_ptr<const scene::ImageResource>>;
using ProviderResourceMap = std::unordered_map<std::string, ResourceMap>;

std::string provider_key(const std::string& application_id, const std::string& instance_id)
{
    std::string key;
    key.reserve(application_id.size() + instance_id.size() + 1);
    key.append(application_id);
    key.push_back('\0');
    key.append(instance_id);
    return key;
}

template <typename Message>
const Message* verified_message(protocol::ByteView payload)
{
    if (payload.data == nullptr || payload.size == 0) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<Message>(nullptr)) {
        return nullptr;
    }
    return flatbuffers::GetRoot<Message>(payload.data);
}

std::shared_ptr<const scene::SceneSnapshot> make_snapshot(
    std::uint64_t revision,
    const ProviderMap& providers)
{
    std::vector<std::shared_ptr<const scene::ProviderScene>> ordered;
    ordered.reserve(providers.size());
    for (const auto& entry : providers) {
        ordered.push_back(entry.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left->identity.application_id < right->identity.application_id
            || (left->identity.application_id == right->identity.application_id
                && left->identity.instance_id < right->identity.instance_id);
    });
    return std::make_shared<const scene::SceneSnapshot>(
        scene::SceneSnapshot { revision, std::move(ordered) });
}

scene::ResourceId element_resource_id(const scene::Element& element)
{
    if (const auto* image = std::get_if<scene::ImageElement>(&element.content)) {
        return image->resource_id;
    }
    if (const auto* gif = std::get_if<scene::GifElement>(&element.content)) {
        return gif->resource_id;
    }
    return 0;
}

std::shared_ptr<const scene::ProviderScene> attach_resources(
    const std::shared_ptr<const scene::ProviderScene>& decoded,
    const ResourceMap* resources,
    const scene::SceneLimits& limits)
{
    if (decoded == nullptr) {
        return nullptr;
    }
    std::unordered_map<scene::ResourceId, bool> referenced;
    for (const auto& element : decoded->elements) {
        const auto resource_id = element_resource_id(element);
        if (resource_id != 0) {
            referenced.emplace(resource_id, true);
        }
    }
    const std::size_t available = resources == nullptr ? 0 : resources->size();
    if (available != referenced.size()) {
        return nullptr;
    }

    auto provider = std::make_shared<scene::ProviderScene>(*decoded);
    provider->resources.reserve(referenced.size());
    for (const auto& entry : referenced) {
        const auto resource = resources->find(entry.first);
        if (resource == resources->end() || resource->second == nullptr
            || !scene::valid_image_resource(*resource->second, limits)) {
            return nullptr;
        }
        provider->resources.push_back(resource->second);
    }
    std::sort(
        provider->resources.begin(),
        provider->resources.end(),
        [](const auto& left, const auto& right) { return left->id < right->id; });

    for (const auto& element : provider->elements) {
        const auto* gif = std::get_if<scene::GifElement>(&element.content);
        if (gif == nullptr) {
            continue;
        }
        const auto resource = resources->find(gif->resource_id);
        if (resource == resources->end()
            || resource->second->decoded.frame_count() < 2
            || gif->frame_index >= resource->second->decoded.frame_count()) {
            return nullptr;
        }
    }
    return provider;
}

bool valid_provider_key_part(const flatbuffers::String* value)
{
    return value != nullptr && value->size() != 0 && value->size() <= 128;
}

bool add_with_limit(std::size_t& total, std::size_t value, std::size_t maximum)
{
    if (total > maximum || value > maximum - total) {
        return false;
    }
    total += value;
    return true;
}

bool resource_maps_fit(
    const ProviderResourceMap& providers,
    const scene::SceneLimits& limits)
{
    std::size_t global_encoded = 0;
    std::size_t global_decoded = 0;
    for (const auto& provider : providers) {
        if (provider.second.size() > limits.maximum_resources_per_provider) {
            return false;
        }
        std::size_t provider_encoded = 0;
        std::size_t provider_decoded = 0;
        for (const auto& entry : provider.second) {
            if (entry.second == nullptr
                || !add_with_limit(
                    provider_encoded,
                    entry.second->encoded.size(),
                    limits.maximum_encoded_resource_bytes_per_provider)
                || !add_with_limit(
                    provider_decoded,
                    entry.second->decoded.rgba.size(),
                    limits.maximum_decoded_resource_bytes_per_provider)
                || !add_with_limit(
                    global_encoded,
                    entry.second->encoded.size(),
                    limits.maximum_encoded_resource_bytes_global)
                || !add_with_limit(
                    global_decoded,
                    entry.second->decoded.rgba.size(),
                    limits.maximum_decoded_resource_bytes_global)) {
                return false;
            }
        }
    }
    return true;
}

bool provider_scenes_fit(
    const ProviderMap& providers,
    const scene::SceneLimits& limits)
{
    ProviderResourceMap resources;
    for (const auto& provider : providers) {
        if (provider.second == nullptr) {
            return false;
        }
        auto& destination = resources[provider.first];
        for (const auto& resource : provider.second->resources) {
            if (resource == nullptr
                || !destination.emplace(resource->id, resource).second) {
                return false;
            }
        }
    }
    return resource_maps_fit(resources, limits);
}

} // namespace

struct SceneMirror::Impl {
    struct PendingSnapshot {
        std::uint64_t revision;
        std::uint32_t expected_providers;
        ProviderMap providers;
        ProviderResourceMap resources;
    };

    Impl(protocol::ProtocolVersion configured_version, scene::SceneLimits configured_limits)
        : version(configured_version)
        , limits(configured_limits)
        , published(std::make_shared<const scene::SceneSnapshot>(
              scene::SceneSnapshot { 0, {} }))
    {
    }

    void publish(std::uint64_t revision)
    {
        auto next = make_snapshot(revision, providers);
        std::atomic_store_explicit(&published, std::move(next), std::memory_order_release);
    }

    protocol::ProtocolVersion version;
    scene::SceneLimits limits;
    ProviderMap providers;
    std::optional<PendingSnapshot> pending;
    std::uint64_t staged_revision = 0;
    ProviderResourceMap staged_resources;
    std::shared_ptr<const scene::SceneSnapshot> published;
};

SceneMirror::SceneMirror(protocol::ProtocolVersion version, scene::SceneLimits limits)
    : impl_(std::make_unique<Impl>(version, limits))
{
}

SceneMirror::~SceneMirror() = default;
SceneMirror::SceneMirror(SceneMirror&&) noexcept = default;
SceneMirror& SceneMirror::operator=(SceneMirror&&) noexcept = default;

ApplyResult SceneMirror::apply_packet(protocol::ByteView packet_bytes, int attachment_fd)
{
    const auto decoded = protocol::decode_packet(packet_bytes);
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded);
    const bool has_attachment = attachment_fd >= 0;
    const bool expects_attachment = packet != nullptr
        && (packet->header.flags & protocol::packet_flag_file_descriptor) != 0;
    if (packet == nullptr
        || (packet->header.flags & ~protocol::packet_flag_file_descriptor) != 0
        || has_attachment != expects_attachment
        || (expects_attachment
            && packet->header.message_type != protocol::MessageType::resource_available)
        || packet->header.version.major != impl_->version.major
        || packet->header.version.minor != impl_->version.minor) {
        return ApplyResult::invalid_packet;
    }

    switch (packet->header.message_type) {
    case protocol::MessageType::scene_snapshot_begin: {
        const auto* begin = verified_message<MangoOverlay::Wire::SceneSnapshotBegin>(
            packet->payload);
        if (begin == nullptr || begin->provider_count() > impl_->limits.maximum_providers) {
            return ApplyResult::invalid_payload;
        }
        impl_->pending = Impl::PendingSnapshot {
            begin->revision(), begin->provider_count(), {}, {} };
        impl_->staged_revision = 0;
        impl_->staged_resources.clear();
        return ApplyResult::accepted;
    }
    case protocol::MessageType::resource_available: {
        const auto* message = verified_message<MangoOverlay::Wire::ResourceAvailable>(
            packet->payload);
        if (message == nullptr || message->resource_id() == 0
            || message->encoded_size() == 0
            || !valid_provider_key_part(message->application_id())
            || !valid_provider_key_part(message->instance_id())) {
            return ApplyResult::invalid_payload;
        }

        ProviderResourceMap* destination = nullptr;
        if (impl_->pending.has_value()) {
            if (message->revision() != impl_->pending->revision) {
                return ApplyResult::invalid_payload;
            }
            destination = &impl_->pending->resources;
        } else {
            const auto current = snapshot()->revision;
            if (message->revision() != current + 1
                || (impl_->staged_revision != 0
                    && impl_->staged_revision != message->revision())) {
                return ApplyResult::revision_gap;
            }
            impl_->staged_revision = message->revision();
            destination = &impl_->staged_resources;
        }

        std::vector<std::uint8_t> encoded;
        const auto* inline_data = message->inline_data();
        if (expects_attachment) {
            if ((inline_data != nullptr && inline_data->size() != 0)
                || !protocol::read_resource_descriptor(
                    attachment_fd,
                    message->encoded_size(),
                    impl_->limits.maximum_encoded_resource_bytes,
                    encoded)) {
                return ApplyResult::invalid_payload;
            }
        } else {
            if (inline_data == nullptr
                || inline_data->size() != message->encoded_size()
                || inline_data->size() > impl_->limits.maximum_encoded_resource_bytes) {
                return ApplyResult::invalid_payload;
            }
            encoded.assign(inline_data->begin(), inline_data->end());
        }

        resource::ImageLimits image_limits;
        image_limits.maximum_encoded_bytes
            = impl_->limits.maximum_encoded_resource_bytes;
        image_limits.maximum_decoded_bytes
            = impl_->limits.maximum_decoded_resource_bytes;
        auto decoded_image = resource::decode_image(
            resource::EncodedView { encoded.data(), encoded.size() }, image_limits);
        auto* image = std::get_if<resource::DecodedImage>(&decoded_image);
        if (image == nullptr) {
            return ApplyResult::invalid_payload;
        }
        auto resource = std::make_shared<const scene::ImageResource>(
            scene::ImageResource {
                message->resource_id(), std::move(encoded), std::move(*image) });
        if (!scene::valid_image_resource(*resource, impl_->limits)) {
            return ApplyResult::invalid_payload;
        }

        const auto key = provider_key(
            message->application_id()->str(), message->instance_id()->str());
        auto& provider_resources = (*destination)[key];
        if (provider_resources.size() >= impl_->limits.maximum_resources_per_provider
            || !provider_resources.emplace(resource->id, resource).second) {
            return ApplyResult::invalid_payload;
        }
        if (!resource_maps_fit(*destination, impl_->limits)) {
            provider_resources.erase(resource->id);
            if (provider_resources.empty()) {
                destination->erase(key);
            }
            return ApplyResult::invalid_payload;
        }
        return ApplyResult::accepted;
    }
    case protocol::MessageType::scene_snapshot_provider: {
        const auto* message
            = verified_message<MangoOverlay::Wire::SceneSnapshotProvider>(packet->payload);
        if (message == nullptr || !impl_->pending.has_value()
            || message->revision() != impl_->pending->revision
            || impl_->pending->providers.size() >= impl_->pending->expected_providers) {
            return ApplyResult::invalid_payload;
        }
        auto decoded_provider = wire::decode_provider(message->provider(), impl_->limits);
        if (decoded_provider == nullptr) {
            return ApplyResult::invalid_payload;
        }
        const auto key = provider_key(
            decoded_provider->identity.application_id,
            decoded_provider->identity.instance_id);
        const auto resources = impl_->pending->resources.find(key);
        auto provider = attach_resources(
            decoded_provider,
            resources == impl_->pending->resources.end() ? nullptr : &resources->second,
            impl_->limits);
        if (provider == nullptr
            || !impl_->pending->providers.emplace(key, std::move(provider)).second) {
            return ApplyResult::invalid_payload;
        }
        if (resources != impl_->pending->resources.end()) {
            impl_->pending->resources.erase(resources);
        }
        return ApplyResult::accepted;
    }
    case protocol::MessageType::scene_snapshot_end: {
        const auto* end = verified_message<MangoOverlay::Wire::SceneSnapshotEnd>(
            packet->payload);
        if (end == nullptr || !impl_->pending.has_value()
            || end->revision() != impl_->pending->revision
            || impl_->pending->providers.size() != impl_->pending->expected_providers
            || !impl_->pending->resources.empty()
            || !provider_scenes_fit(impl_->pending->providers, impl_->limits)) {
            return ApplyResult::invalid_payload;
        }
        impl_->providers = std::move(impl_->pending->providers);
        const auto revision = impl_->pending->revision;
        impl_->pending.reset();
        impl_->publish(revision);
        return ApplyResult::published;
    }
    case protocol::MessageType::provider_scene_updated: {
        const auto* update = verified_message<MangoOverlay::Wire::ProviderSceneUpdated>(
            packet->payload);
        const auto current = snapshot()->revision;
        if (update == nullptr) {
            return ApplyResult::invalid_payload;
        }
        if (update->revision() != current + 1) {
            return ApplyResult::revision_gap;
        }
        auto decoded_provider = wire::decode_provider(update->provider(), impl_->limits);
        if (decoded_provider == nullptr) {
            return ApplyResult::invalid_payload;
        }
        const auto key = provider_key(
            decoded_provider->identity.application_id,
            decoded_provider->identity.instance_id);
        if (impl_->staged_revision != 0
            && impl_->staged_revision != update->revision()) {
            return ApplyResult::revision_gap;
        }
        const auto resources = impl_->staged_resources.find(key);
        const std::size_t expected_resource_maps
            = resources == impl_->staged_resources.end() ? 0 : 1;
        if (impl_->staged_resources.size() != expected_resource_maps) {
            return ApplyResult::invalid_payload;
        }
        auto provider = attach_resources(
            decoded_provider,
            resources == impl_->staged_resources.end() ? nullptr : &resources->second,
            impl_->limits);
        if (provider == nullptr) {
            return ApplyResult::invalid_payload;
        }
        auto providers = impl_->providers;
        providers.insert_or_assign(key, std::move(provider));
        if (!provider_scenes_fit(providers, impl_->limits)) {
            return ApplyResult::invalid_payload;
        }
        impl_->pending.reset();
        impl_->staged_revision = 0;
        impl_->staged_resources.clear();
        impl_->providers = std::move(providers);
        impl_->publish(update->revision());
        return ApplyResult::published;
    }
    case protocol::MessageType::provider_scene_removed: {
        const auto* removal = verified_message<MangoOverlay::Wire::ProviderSceneRemoved>(
            packet->payload);
        const auto current = snapshot()->revision;
        if (removal == nullptr || removal->application_id() == nullptr
            || removal->instance_id() == nullptr) {
            return ApplyResult::invalid_payload;
        }
        if (removal->revision() != current + 1) {
            return ApplyResult::revision_gap;
        }
        if (impl_->staged_revision != 0 || !impl_->staged_resources.empty()) {
            return ApplyResult::invalid_payload;
        }
        impl_->pending.reset();
        impl_->providers.erase(provider_key(
            removal->application_id()->str(), removal->instance_id()->str()));
        impl_->publish(removal->revision());
        return ApplyResult::published;
    }
    default:
        return ApplyResult::unexpected_message;
    }
}

std::shared_ptr<const scene::SceneSnapshot> SceneMirror::snapshot() const
{
    return std::atomic_load_explicit(&impl_->published, std::memory_order_acquire);
}

} // namespace mango_overlay::renderer
