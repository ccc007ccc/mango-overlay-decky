#pragma once

#include "mango_overlay/scene/store.hpp"

namespace mango_overlay::scene {

bool valid_provider_identity(const ProviderIdentity& identity);
bool valid_scene_element(const Element& element, const SceneLimits& limits);
bool valid_image_resource(const ImageResource& resource, const SceneLimits& limits);
bool provider_scene_fits_size_limit(
    const ProviderScene& provider,
    const SceneLimits& limits);
bool valid_provider_scene(const ProviderScene& provider, const SceneLimits& limits);

} // namespace mango_overlay::scene
