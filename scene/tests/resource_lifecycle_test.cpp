#include "mango_overlay/scene/store.hpp"

#include <cstdio>
#include <memory>

using namespace mango_overlay::scene;

namespace {

std::shared_ptr<const ImageResource> resource(
    ResourceId id,
    std::size_t frames)
{
    mango_overlay::resource::DecodedImage decoded {
        frames == 1
            ? mango_overlay::resource::ImageFormat::png
            : mango_overlay::resource::ImageFormat::gif,
        1,
        1,
        std::vector<std::uint32_t>(frames, 20),
        std::vector<std::uint8_t>(frames * 4, 255),
    };
    return std::make_shared<const ImageResource>(ImageResource {
        id,
        std::vector<std::uint8_t> { 1, 2, 3, 4 },
        std::move(decoded),
    });
}

Element image_element(ElementId id, ResourceId resource_id)
{
    return Element {
        id,
        0,
        ImageElement {
            { 10.0F, 20.0F },
            { 100.0F, 80.0F },
            resource_id,
            { 1.0F, 1.0F, 1.0F, 1.0F },
        },
    };
}

Element gif_element(ElementId id, ResourceId resource_id)
{
    return Element {
        id,
        1,
        GifElement {
            { 120.0F, 20.0F },
            { 100.0F, 80.0F },
            resource_id,
            { 1.0F, 1.0F, 1.0F, 1.0F },
            1.0F,
            false,
            0,
        },
    };
}

} // namespace

int main()
{
    SceneStore store;
    store.register_provider(
        1,
        ProviderIdentity {
            "resource.test", "primary", "Resources", 1280, 800, Visibility::always });

    if (store.commit(
            1,
            SceneTransaction { 1, { UpsertElement { image_element(1, 10) } } })
        != CommitResult::resource_missing) {
        std::fputs("scene accepted a missing image resource\n", stderr);
        return 1;
    }
    if (store.store_resource(1, resource(10, 1)) != ResourceResult::stored
        || store.store_resource(1, resource(11, 2)) != ResourceResult::stored) {
        std::fputs("valid resources were rejected\n", stderr);
        return 1;
    }
    if (store.commit(
            1,
            SceneTransaction {
                2,
                {
                    UpsertElement { image_element(1, 10) },
                    UpsertElement { gif_element(2, 11) },
                },
            })
            != CommitResult::applied
        || store.snapshot()->providers[0]->resources.size() != 2) {
        std::fputs("scene did not publish its referenced resources\n", stderr);
        return 1;
    }
    if (store.store_resource(1, resource(10, 1)) != ResourceResult::resource_in_use
        || store.release_resource(1, 11) != ResourceResult::resource_in_use) {
        std::fputs("an in-use resource was replaced or released\n", stderr);
        return 1;
    }
    if (store.commit(
            1,
            SceneTransaction { 3, { UpsertElement { gif_element(2, 10) } } })
        != CommitResult::invalid_resource) {
        std::fputs("GIF element accepted a static image resource\n", stderr);
        return 1;
    }
    if (store.commit(
            1,
            SceneTransaction {
                4,
                {
                    RemoveElement { 1 },
                    RemoveElement { 2 },
                },
            })
            != CommitResult::applied
        || store.release_resource(1, 10) != ResourceResult::released
        || store.release_resource(1, 11) != ResourceResult::released) {
        std::fputs("unused resources could not be released\n", stderr);
        return 1;
    }

    SceneLimits limits;
    limits.maximum_encoded_resource_bytes_per_provider = 8;
    limits.maximum_decoded_resource_bytes_per_provider = 8;
    limits.maximum_encoded_resource_bytes_global = 12;
    limits.maximum_decoded_resource_bytes_global = 12;
    SceneStore bounded(limits);
    bounded.register_provider(
        1,
        ProviderIdentity {
            "resource.limit", "first", "First", 1280, 800, Visibility::always });
    bounded.register_provider(
        2,
        ProviderIdentity {
            "resource.limit", "second", "Second", 1280, 800, Visibility::always });
    if (bounded.store_resource(1, resource(10, 2)) != ResourceResult::stored
        || bounded.store_resource(2, resource(20, 1)) != ResourceResult::stored
        || bounded.store_resource(2, resource(21, 1))
            != ResourceResult::resource_limit_reached) {
        std::fputs("global resource budget was not enforced\n", stderr);
        return 1;
    }
    if (bounded.store_resource(1, resource(10, 1)) != ResourceResult::stored
        || bounded.store_resource(2, resource(21, 1)) != ResourceResult::stored
        || bounded.store_resource(1, resource(11, 1))
            != ResourceResult::resource_limit_reached) {
        std::fputs("resource replacement did not update the global budget\n", stderr);
        return 1;
    }
    bounded.disconnect(2);
    if (bounded.store_resource(1, resource(11, 1)) != ResourceResult::stored) {
        std::fputs("provider disconnect did not release the global budget\n", stderr);
        return 1;
    }
    return 0;
}
