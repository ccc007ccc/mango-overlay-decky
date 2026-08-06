#pragma once

#include "mango_overlay/resource/image.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mango_overlay::scene {

using ConnectionId = std::uint64_t;
using ElementId = std::uint64_t;
using ResourceId = std::uint64_t;

enum class Visibility : std::uint8_t {
    game_only = 0,
    steam_only = 1,
    always = 2,
};

struct Vec2 {
    float x;
    float y;
};

struct Color {
    float red;
    float green;
    float blue;
    float alpha;
};

enum class Anchor : std::uint8_t {
    top_left = 0,
    top_center = 1,
    top_right = 2,
    center_left = 3,
    center = 4,
    center_right = 5,
    bottom_left = 6,
    bottom_center = 7,
    bottom_right = 8,
};

struct ClipRect {
    Vec2 position;
    Vec2 size;
};

struct ElementTransform {
    Vec2 translation { 0.0F, 0.0F };
    Vec2 scale { 1.0F, 1.0F };
    float rotation_degrees = 0.0F;
    float opacity = 1.0F;
    Anchor anchor = Anchor::top_left;
    std::optional<ClipRect> clip;
    bool hidden = false;
};

struct GroupElement {
};

struct TextElement {
    Vec2 position;
    std::string text;
    float font_size;
    Color color;
};

struct RectangleElement {
    Vec2 position;
    Vec2 size;
    float corner_radius;
    Color color;
};

struct LineElement {
    Vec2 start;
    Vec2 end;
    float thickness;
    Color color;
};

struct PolylineElement {
    std::vector<Vec2> points;
    float thickness;
    Color color;
};

struct CircleElement {
    Vec2 center;
    float radius;
    Color color;
};

struct ImageElement {
    Vec2 position;
    Vec2 size;
    ResourceId resource_id;
    Color tint;
};

struct GifElement {
    Vec2 position;
    Vec2 size;
    ResourceId resource_id;
    Color tint;
    float playback_rate;
    bool paused;
    std::uint32_t frame_index;
};

using ElementContent = std::variant<
    GroupElement,
    TextElement,
    RectangleElement,
    LineElement,
    PolylineElement,
    CircleElement,
    ImageElement,
    GifElement>;

struct ImageResource {
    ResourceId id;
    std::vector<std::uint8_t> encoded;
    resource::DecodedImage decoded;
};

struct Element {
    ElementId id;
    std::int32_t z_index;
    ElementContent content;
    ElementId parent_id = 0;
    ElementTransform transform;
};

struct UpsertElement {
    Element element;
};

struct RemoveElement {
    ElementId id;
};

using SceneMutation = std::variant<UpsertElement, RemoveElement>;

struct SceneTransaction {
    std::uint64_t transaction_id;
    std::vector<SceneMutation> mutations;
};

struct ProviderIdentity {
    std::string application_id;
    std::string instance_id;
    std::string display_name;
    std::uint16_t canvas_width;
    std::uint16_t canvas_height;
    Visibility requested_visibility;
};

struct ProviderScene {
    ProviderIdentity identity;
    std::vector<Element> elements;
    std::vector<std::shared_ptr<const ImageResource>> resources;
};

struct SceneSnapshot {
    std::uint64_t revision;
    std::vector<std::shared_ptr<const ProviderScene>> providers;
};

struct ApplicationPolicy {
    std::string application_id;
    std::string display_name;
    bool approved;
    bool visible;
    std::int32_t order;
    std::uint32_t active_instances = 0;
};

struct OverlayPolicy {
    bool enabled = true;
    bool require_approval = false;
    std::vector<ApplicationPolicy> applications;
};

struct ApplicationPolicyUpdate {
    bool approved;
    bool visible;
    std::int32_t order;
};

using PolicyCommit = std::function<bool(const OverlayPolicy&)>;

struct SceneLimits {
    std::size_t maximum_providers = 32;
    std::size_t maximum_elements_per_provider = 512;
    std::size_t maximum_mutations_per_transaction = 256;
    std::size_t maximum_text_bytes = 4096;
    std::size_t maximum_polyline_points = 1024;
    std::size_t maximum_group_depth = 16;
    std::size_t maximum_children_per_group = 128;
    std::size_t maximum_resources_per_provider = 32;
    std::size_t maximum_encoded_resource_bytes = 8 * 1024 * 1024;
    std::size_t maximum_decoded_resource_bytes = 32 * 1024 * 1024;
    std::size_t maximum_encoded_resource_bytes_per_provider = 32 * 1024 * 1024;
    std::size_t maximum_decoded_resource_bytes_per_provider = 64 * 1024 * 1024;
    std::size_t maximum_encoded_resource_bytes_global = 128 * 1024 * 1024;
    std::size_t maximum_decoded_resource_bytes_global = 128 * 1024 * 1024;
    std::size_t maximum_scene_bytes_per_provider = 512 * 1024;
    std::size_t maximum_retained_changes = 64;
};

enum class SceneChangeKind {
    upsert,
    remove_provider,
    reset,
};

struct SceneChange {
    std::uint64_t revision;
    SceneChangeKind kind;
    ProviderIdentity identity;
    std::shared_ptr<const ProviderScene> provider;
};

struct SceneChangeBatch {
    bool history_gap;
    std::vector<SceneChange> changes;
};

enum class RegistrationResult {
    registered,
    invalid_identity,
    duplicate_connection,
    duplicate_instance,
    provider_limit_reached,
};

enum class CommitResult {
    applied,
    already_applied,
    provider_not_registered,
    invalid_transaction,
    stale_transaction,
    invalid_element,
    resource_missing,
    invalid_resource,
    element_limit_reached,
    scene_size_limit_reached,
};

enum class ResourceResult {
    stored,
    released,
    provider_not_registered,
    invalid_resource,
    resource_limit_reached,
    resource_in_use,
    resource_not_found,
};

class SceneStore {
public:
    explicit SceneStore(SceneLimits limits = {}, OverlayPolicy policy = {});
    ~SceneStore();

    SceneStore(SceneStore&&) noexcept;
    SceneStore& operator=(SceneStore&&) noexcept;
    SceneStore(const SceneStore&) = delete;
    SceneStore& operator=(const SceneStore&) = delete;

    RegistrationResult register_provider(ConnectionId connection, ProviderIdentity identity);
    ResourceResult store_resource(
        ConnectionId connection,
        std::shared_ptr<const ImageResource> resource);
    ResourceResult release_resource(ConnectionId connection, ResourceId resource_id);
    CommitResult commit(ConnectionId connection, const SceneTransaction& transaction);
    void disconnect(ConnectionId connection);
    bool set_enabled(bool enabled, const PolicyCommit& commit = {});
    bool set_require_approval(bool required, const PolicyCommit& commit = {});
    bool set_application_policy(
        const std::string& application_id,
        ApplicationPolicyUpdate update,
        const PolicyCommit& commit = {});
    bool set_application_position(
        const std::string& application_id,
        std::uint32_t position,
        const PolicyCommit& commit = {});
    OverlayPolicy policy() const;
    SceneLimits limits() const;
    std::shared_ptr<const SceneSnapshot> snapshot() const;
    SceneChangeBatch changes_after(std::uint64_t revision) const;
    SceneChangeBatch wait_for_changes_after(
        std::uint64_t revision,
        std::chrono::milliseconds timeout) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mango_overlay::scene
