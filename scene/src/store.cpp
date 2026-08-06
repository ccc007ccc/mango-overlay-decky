#include "mango_overlay/scene/store.hpp"
#include "mango_overlay/scene/validation.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace mango_overlay::scene {

namespace {

std::int32_t next_policy_order(std::int32_t order)
{
    return order == std::numeric_limits<std::int32_t>::max() ? order : order + 1;
}

} // namespace

struct SceneStore::Impl {
    struct ProviderState {
        ProviderIdentity identity;
        std::unordered_map<ElementId, Element> elements;
        std::unordered_map<ResourceId, std::shared_ptr<const ImageResource>> resources;
        std::size_t encoded_resource_bytes = 0;
        std::size_t decoded_resource_bytes = 0;
        std::uint64_t last_transaction_id = 0;
        std::shared_ptr<const ProviderScene> published;
    };

    explicit Impl(SceneLimits configured_limits, OverlayPolicy configured_policy)
        : limits(configured_limits)
        , enabled(configured_policy.enabled)
        , require_approval(configured_policy.require_approval)
        , current_snapshot(std::make_shared<const SceneSnapshot>(SceneSnapshot { 0, {} }))
    {
        for (auto& application : configured_policy.applications) {
            application.active_instances = 0;
            next_order = std::max(next_order, next_policy_order(application.order));
            policies.insert_or_assign(application.application_id, std::move(application));
        }
    }

    bool provider_visible_locked(const ProviderIdentity& identity) const
    {
        if (!enabled) {
            return false;
        }
        const auto policy = policies.find(identity.application_id);
        return policy != policies.end() && policy->second.approved && policy->second.visible;
    }

    void publish_locked()
    {
        std::vector<std::shared_ptr<const ProviderScene>> scenes;
        scenes.reserve(providers.size());
        for (const auto& entry : providers) {
            if (provider_visible_locked(entry.second.identity)) {
                scenes.push_back(entry.second.published);
            }
        }
        std::sort(scenes.begin(), scenes.end(), [&](const auto& left, const auto& right) {
            const auto left_order = policies.at(left->identity.application_id).order;
            const auto right_order = policies.at(right->identity.application_id).order;
            if (left_order != right_order) {
                return left_order < right_order;
            }
            if (left->identity.application_id != right->identity.application_id) {
                return left->identity.application_id < right->identity.application_id;
            }
            return left->identity.instance_id < right->identity.instance_id;
        });
        current_snapshot = std::make_shared<const SceneSnapshot>(
            SceneSnapshot { revision, std::move(scenes) });
    }

    void record_change_locked(
        SceneChangeKind kind,
        const ProviderIdentity& identity,
        std::shared_ptr<const ProviderScene> provider)
    {
        ++revision;
        changes.push_back(SceneChange { revision, kind, identity, std::move(provider) });
        while (changes.size() > std::max<std::size_t>(1, limits.maximum_retained_changes)) {
            changes.pop_front();
        }
        publish_locked();
        changed.notify_all();
    }

    void record_reset_locked()
    {
        ++revision;
        changes.push_back(SceneChange {
            revision,
            SceneChangeKind::reset,
            ProviderIdentity {},
            nullptr,
        });
        while (changes.size() > std::max<std::size_t>(1, limits.maximum_retained_changes)) {
            changes.pop_front();
        }
        publish_locked();
        changed.notify_all();
    }

    OverlayPolicy policy_locked() const
    {
        OverlayPolicy result;
        result.enabled = enabled;
        result.require_approval = require_approval;
        result.applications.reserve(policies.size());
        for (const auto& entry : policies) {
            auto application = entry.second;
            application.active_instances = static_cast<std::uint32_t>(std::count_if(
                providers.begin(),
                providers.end(),
                [&](const auto& provider) {
                    return provider.second.identity.application_id
                        == application.application_id;
                }));
            result.applications.push_back(std::move(application));
        }
        std::sort(
            result.applications.begin(),
            result.applications.end(),
            [](const ApplicationPolicy& left, const ApplicationPolicy& right) {
                if (left.order != right.order) {
                    return left.order < right.order;
                }
                return left.application_id < right.application_id;
            });
        return result;
    }

    SceneChangeBatch changes_after_locked(std::uint64_t known_revision) const
    {
        if (known_revision >= revision) {
            return { false, {} };
        }
        if (changes.empty() || known_revision + 1 < changes.front().revision) {
            return { true, {} };
        }

        std::vector<SceneChange> result;
        for (const auto& change : changes) {
            if (change.revision > known_revision) {
                result.push_back(change);
            }
        }
        return { false, std::move(result) };
    }

    SceneLimits limits;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::unordered_map<ConnectionId, ProviderState> providers;
    std::unordered_map<std::string, ApplicationPolicy> policies;
    std::deque<SceneChange> changes;
    bool enabled = true;
    bool require_approval = false;
    std::int32_t next_order = 0;
    std::uint64_t revision = 0;
    std::size_t encoded_resource_bytes = 0;
    std::size_t decoded_resource_bytes = 0;
    std::shared_ptr<const SceneSnapshot> current_snapshot;
};

namespace {

std::shared_ptr<const ProviderScene> make_published_scene(
    const ProviderIdentity& identity,
    const std::unordered_map<ElementId, Element>& elements,
    const std::unordered_map<ResourceId, std::shared_ptr<const ImageResource>>& resources)
{
    std::vector<Element> ordered_elements;
    ordered_elements.reserve(elements.size());
    for (const auto& entry : elements) {
        ordered_elements.push_back(entry.second);
    }
    std::sort(ordered_elements.begin(), ordered_elements.end(), [](const Element& left, const Element& right) {
        if (left.z_index != right.z_index) {
            return left.z_index < right.z_index;
        }
        return left.id < right.id;
    });
    std::vector<std::shared_ptr<const ImageResource>> referenced_resources;
    std::unordered_map<ResourceId, bool> referenced;
    for (const auto& element : ordered_elements) {
        ResourceId resource_id = 0;
        if (const auto* image = std::get_if<ImageElement>(&element.content)) {
            resource_id = image->resource_id;
        } else if (const auto* gif = std::get_if<GifElement>(&element.content)) {
            resource_id = gif->resource_id;
        }
        if (resource_id == 0 || !referenced.emplace(resource_id, true).second) {
            continue;
        }
        const auto resource = resources.find(resource_id);
        if (resource != resources.end()) {
            referenced_resources.push_back(resource->second);
        }
    }
    std::sort(
        referenced_resources.begin(),
        referenced_resources.end(),
        [](const auto& left, const auto& right) { return left->id < right->id; });
    return std::make_shared<const ProviderScene>(ProviderScene {
        identity, std::move(ordered_elements), std::move(referenced_resources) });
}

bool resource_is_referenced(
    const std::unordered_map<ElementId, Element>& elements,
    ResourceId resource_id)
{
    return std::any_of(elements.begin(), elements.end(), [&](const auto& entry) {
        if (const auto* image = std::get_if<ImageElement>(&entry.second.content)) {
            return image->resource_id == resource_id;
        }
        if (const auto* gif = std::get_if<GifElement>(&entry.second.content)) {
            return gif->resource_id == resource_id;
        }
        return false;
    });
}

bool fits_replacement(
    std::size_t current,
    std::size_t replaced,
    std::size_t replacement,
    std::size_t maximum)
{
    if (replaced > current) {
        return false;
    }
    const auto retained = current - replaced;
    return retained <= maximum && replacement <= maximum - retained;
}

} // namespace

SceneStore::SceneStore(SceneLimits limits, OverlayPolicy policy)
    : impl_(std::make_unique<Impl>(limits, std::move(policy)))
{
}

SceneStore::~SceneStore() = default;
SceneStore::SceneStore(SceneStore&&) noexcept = default;
SceneStore& SceneStore::operator=(SceneStore&&) noexcept = default;

RegistrationResult SceneStore::register_provider(ConnectionId connection, ProviderIdentity identity)
{
    if (!valid_provider_identity(identity)) {
        return RegistrationResult::invalid_identity;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->providers.find(connection) != impl_->providers.end()) {
        return RegistrationResult::duplicate_connection;
    }
    for (const auto& entry : impl_->providers) {
        if (entry.second.identity.application_id == identity.application_id
            && entry.second.identity.instance_id == identity.instance_id) {
            return RegistrationResult::duplicate_instance;
        }
    }
    if (impl_->providers.size() >= impl_->limits.maximum_providers) {
        return RegistrationResult::provider_limit_reached;
    }

    const auto existing_policy = impl_->policies.find(identity.application_id);
    if (existing_policy == impl_->policies.end()) {
        impl_->policies.emplace(
            identity.application_id,
            ApplicationPolicy {
                identity.application_id,
                identity.display_name,
                !impl_->require_approval,
                true,
                impl_->next_order,
                0,
            });
        impl_->next_order = next_policy_order(impl_->next_order);
    } else {
        existing_policy->second.display_name = identity.display_name;
    }

    Impl::ProviderState provider;
    provider.identity = std::move(identity);
    provider.published = make_published_scene(
        provider.identity, provider.elements, provider.resources);
    const auto published = provider.published;
    const auto identity_copy = provider.identity;
    impl_->providers.emplace(connection, std::move(provider));
    if (impl_->provider_visible_locked(identity_copy)) {
        impl_->record_change_locked(SceneChangeKind::upsert, identity_copy, published);
    }
    return RegistrationResult::registered;
}

ResourceResult SceneStore::store_resource(
    ConnectionId connection,
    std::shared_ptr<const ImageResource> resource)
{
    if (resource == nullptr || !valid_image_resource(*resource, impl_->limits)) {
        return ResourceResult::invalid_resource;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto provider_entry = impl_->providers.find(connection);
    if (provider_entry == impl_->providers.end()) {
        return ResourceResult::provider_not_registered;
    }
    auto& provider = provider_entry->second;
    const auto existing = provider.resources.find(resource->id);
    if (existing != provider.resources.end()
        && resource_is_referenced(provider.elements, resource->id)) {
        return ResourceResult::resource_in_use;
    }
    if (existing == provider.resources.end()
        && provider.resources.size() >= impl_->limits.maximum_resources_per_provider) {
        return ResourceResult::resource_limit_reached;
    }

    const auto replaced_encoded = existing == provider.resources.end()
        ? 0
        : existing->second->encoded.size();
    const auto replaced_decoded = existing == provider.resources.end()
        ? 0
        : existing->second->decoded.rgba.size();
    const auto encoded = resource->encoded.size();
    const auto decoded = resource->decoded.rgba.size();
    if (!fits_replacement(
            provider.encoded_resource_bytes,
            replaced_encoded,
            encoded,
            impl_->limits.maximum_encoded_resource_bytes_per_provider)
        || !fits_replacement(
            provider.decoded_resource_bytes,
            replaced_decoded,
            decoded,
            impl_->limits.maximum_decoded_resource_bytes_per_provider)
        || !fits_replacement(
            impl_->encoded_resource_bytes,
            replaced_encoded,
            encoded,
            impl_->limits.maximum_encoded_resource_bytes_global)
        || !fits_replacement(
            impl_->decoded_resource_bytes,
            replaced_decoded,
            decoded,
            impl_->limits.maximum_decoded_resource_bytes_global)) {
        return ResourceResult::resource_limit_reached;
    }
    provider.encoded_resource_bytes
        = provider.encoded_resource_bytes - replaced_encoded + encoded;
    provider.decoded_resource_bytes
        = provider.decoded_resource_bytes - replaced_decoded + decoded;
    impl_->encoded_resource_bytes
        = impl_->encoded_resource_bytes - replaced_encoded + encoded;
    impl_->decoded_resource_bytes
        = impl_->decoded_resource_bytes - replaced_decoded + decoded;
    provider.resources.insert_or_assign(resource->id, std::move(resource));
    return ResourceResult::stored;
}

ResourceResult SceneStore::release_resource(
    ConnectionId connection,
    ResourceId resource_id)
{
    if (resource_id == 0) {
        return ResourceResult::invalid_resource;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto provider_entry = impl_->providers.find(connection);
    if (provider_entry == impl_->providers.end()) {
        return ResourceResult::provider_not_registered;
    }
    auto& provider = provider_entry->second;
    const auto resource = provider.resources.find(resource_id);
    if (resource == provider.resources.end()) {
        return ResourceResult::resource_not_found;
    }
    if (resource_is_referenced(provider.elements, resource_id)) {
        return ResourceResult::resource_in_use;
    }
    provider.encoded_resource_bytes -= resource->second->encoded.size();
    provider.decoded_resource_bytes -= resource->second->decoded.rgba.size();
    impl_->encoded_resource_bytes -= resource->second->encoded.size();
    impl_->decoded_resource_bytes -= resource->second->decoded.rgba.size();
    provider.resources.erase(resource);
    return ResourceResult::released;
}

CommitResult SceneStore::commit(ConnectionId connection, const SceneTransaction& transaction)
{
    if (transaction.transaction_id == 0 || transaction.mutations.empty()
        || transaction.mutations.size() > impl_->limits.maximum_mutations_per_transaction) {
        return CommitResult::invalid_transaction;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto provider_entry = impl_->providers.find(connection);
    if (provider_entry == impl_->providers.end()) {
        return CommitResult::provider_not_registered;
    }
    auto& provider = provider_entry->second;
    if (transaction.transaction_id == provider.last_transaction_id) {
        return CommitResult::already_applied;
    }
    if (transaction.transaction_id < provider.last_transaction_id) {
        return CommitResult::stale_transaction;
    }

    auto candidate = provider.elements;
    for (const auto& mutation : transaction.mutations) {
        if (const auto* upsert = std::get_if<UpsertElement>(&mutation)) {
            if (!valid_scene_element(upsert->element, impl_->limits)) {
                return CommitResult::invalid_element;
            }
            candidate.insert_or_assign(upsert->element.id, upsert->element);
        } else {
            candidate.erase(std::get<RemoveElement>(mutation).id);
        }
    }
    if (candidate.size() > impl_->limits.maximum_elements_per_provider) {
        return CommitResult::element_limit_reached;
    }
    const auto candidate_scene = make_published_scene(
        provider.identity, candidate, provider.resources);
    if (!provider_scene_fits_size_limit(*candidate_scene, impl_->limits)) {
        return CommitResult::scene_size_limit_reached;
    }
    if (!valid_provider_scene(*candidate_scene, impl_->limits)) {
        return CommitResult::invalid_element;
    }
    for (const auto& element : candidate_scene->elements) {
        ResourceId resource_id = 0;
        bool requires_animation = false;
        std::uint32_t frame_index = 0;
        if (const auto* image = std::get_if<ImageElement>(&element.content)) {
            resource_id = image->resource_id;
        } else if (const auto* gif = std::get_if<GifElement>(&element.content)) {
            resource_id = gif->resource_id;
            requires_animation = true;
            frame_index = gif->frame_index;
        }
        if (resource_id == 0) {
            continue;
        }
        const auto resource = provider.resources.find(resource_id);
        if (resource == provider.resources.end()) {
            return CommitResult::resource_missing;
        }
        if (requires_animation
            && (resource->second->decoded.frame_count() < 2
                || frame_index >= resource->second->decoded.frame_count())) {
            return CommitResult::invalid_resource;
        }
    }

    provider.elements = std::move(candidate);
    provider.last_transaction_id = transaction.transaction_id;
    provider.published = candidate_scene;
    if (impl_->provider_visible_locked(provider.identity)) {
        impl_->record_change_locked(
            SceneChangeKind::upsert,
            provider.identity,
            provider.published);
    }
    return CommitResult::applied;
}

void SceneStore::disconnect(ConnectionId connection)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto provider = impl_->providers.find(connection);
    if (provider != impl_->providers.end()) {
        const auto identity = provider->second.identity;
        const bool was_visible = impl_->provider_visible_locked(identity);
        impl_->encoded_resource_bytes -= provider->second.encoded_resource_bytes;
        impl_->decoded_resource_bytes -= provider->second.decoded_resource_bytes;
        impl_->providers.erase(provider);
        if (was_visible) {
            impl_->record_change_locked(SceneChangeKind::remove_provider, identity, nullptr);
        }
    }
}

bool SceneStore::set_enabled(bool enabled, const PolicyCommit& commit)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->enabled == enabled) {
        return true;
    }
    auto candidate = impl_->policy_locked();
    candidate.enabled = enabled;
    if (commit && !commit(candidate)) {
        return false;
    }
    impl_->enabled = enabled;
    impl_->record_reset_locked();
    return true;
}

bool SceneStore::set_require_approval(bool required, const PolicyCommit& commit)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->require_approval == required) {
        return true;
    }
    auto candidate = impl_->policy_locked();
    candidate.require_approval = required;
    if (commit && !commit(candidate)) {
        return false;
    }
    impl_->require_approval = required;
    return true;
}

bool SceneStore::set_application_policy(
    const std::string& application_id,
    ApplicationPolicyUpdate update,
    const PolicyCommit& commit)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto policy = impl_->policies.find(application_id);
    if (policy == impl_->policies.end()) {
        return false;
    }
    if (policy->second.approved == update.approved
        && policy->second.visible == update.visible
        && policy->second.order == update.order) {
        return true;
    }
    auto candidate = impl_->policy_locked();
    const auto candidate_policy = std::find_if(
        candidate.applications.begin(),
        candidate.applications.end(),
        [&](const ApplicationPolicy& application) {
            return application.application_id == application_id;
        });
    if (candidate_policy == candidate.applications.end()) {
        return false;
    }
    candidate_policy->approved = update.approved;
    candidate_policy->visible = update.visible;
    candidate_policy->order = update.order;
    if (commit && !commit(candidate)) {
        return false;
    }
    policy->second.approved = update.approved;
    policy->second.visible = update.visible;
    policy->second.order = update.order;
    impl_->next_order = std::max(impl_->next_order, next_policy_order(update.order));
    impl_->record_reset_locked();
    return true;
}

bool SceneStore::set_application_position(
    const std::string& application_id,
    std::uint32_t position,
    const PolicyCommit& commit)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto candidate = impl_->policy_locked();
    if (position >= candidate.applications.size()) {
        return false;
    }
    const auto current = std::find_if(
        candidate.applications.begin(),
        candidate.applications.end(),
        [&](const ApplicationPolicy& application) {
            return application.application_id == application_id;
        });
    if (current == candidate.applications.end()) {
        return false;
    }
    const auto current_position = static_cast<std::size_t>(
        std::distance(candidate.applications.begin(), current));
    if (current_position == position) {
        return true;
    }
    auto moved = std::move(*current);
    candidate.applications.erase(current);
    candidate.applications.insert(
        candidate.applications.begin() + position, std::move(moved));
    for (std::size_t index = 0; index < candidate.applications.size(); ++index) {
        candidate.applications[index].order = static_cast<std::int32_t>(index);
    }
    if (commit && !commit(candidate)) {
        return false;
    }
    for (const auto& application : candidate.applications) {
        impl_->policies.at(application.application_id).order = application.order;
    }
    impl_->next_order = static_cast<std::int32_t>(candidate.applications.size());
    impl_->record_reset_locked();
    return true;
}

OverlayPolicy SceneStore::policy() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->policy_locked();
}

SceneLimits SceneStore::limits() const
{
    return impl_->limits;
}

std::shared_ptr<const SceneSnapshot> SceneStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current_snapshot;
}

SceneChangeBatch SceneStore::changes_after(std::uint64_t revision) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->changes_after_locked(revision);
}

SceneChangeBatch SceneStore::wait_for_changes_after(
    std::uint64_t revision,
    std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->changed.wait_for(lock, timeout, [&] { return impl_->revision > revision; });
    return impl_->changes_after_locked(revision);
}

} // namespace mango_overlay::scene
