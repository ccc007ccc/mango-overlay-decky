#include "mango_overlay/scene/store.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>

using mango_overlay::scene::ApplicationPolicyUpdate;
using mango_overlay::scene::ProviderIdentity;
using mango_overlay::scene::SceneChangeKind;
using mango_overlay::scene::SceneStore;
using mango_overlay::scene::Visibility;

namespace {

ProviderIdentity provider(std::string application, std::string instance, std::string name)
{
    return {
        std::move(application),
        std::move(instance),
        std::move(name),
        1280,
        800,
        Visibility::always,
    };
}

} // namespace

int main()
{
    mango_overlay::scene::OverlayPolicy saturated_policy;
    saturated_policy.applications.push_back({
        "dev.example.maximum",
        "Maximum",
        true,
        true,
        std::numeric_limits<std::int32_t>::max(),
        0,
    });
    SceneStore saturated_store({}, std::move(saturated_policy));
    saturated_store.register_provider(
        90, provider("dev.example.maximum", "one", "Maximum"));
    saturated_store.register_provider(
        91, provider("dev.example.after-maximum", "one", "After Maximum"));
    const auto saturated_result = saturated_store.policy();
    const auto after_maximum = std::find_if(
        saturated_result.applications.begin(),
        saturated_result.applications.end(),
        [](const auto& application) {
            return application.application_id == "dev.example.after-maximum";
        });
    if (saturated_result.applications.size() != 2
        || after_maximum == saturated_result.applications.end()
        || after_maximum->order
            != std::numeric_limits<std::int32_t>::max()) {
        std::fputs("maximum persisted order overflowed while registering a provider\n", stderr);
        return 1;
    }

    SceneStore store;
    store.register_provider(1, provider("dev.example.first", "one", "First"));
    store.register_provider(2, provider("dev.example.second", "one", "Second"));

    auto snapshot = store.snapshot();
    if (snapshot->providers.size() != 2
        || snapshot->providers[0]->identity.application_id != "dev.example.first"
        || snapshot->providers[1]->identity.application_id != "dev.example.second") {
        std::fputs("default policy did not publish providers deterministically\n", stderr);
        return 1;
    }

    const auto initial_policy = store.policy();
    if (!initial_policy.enabled || initial_policy.require_approval
        || initial_policy.applications.size() != 2
        || !initial_policy.applications[0].approved
        || initial_policy.applications[0].active_instances != 1) {
        std::fputs("default provider policy status is incorrect\n", stderr);
        return 1;
    }

    const auto before_reorder = snapshot->revision;
    if (!store.set_application_policy(
            "dev.example.second", ApplicationPolicyUpdate { true, true, -10 })) {
        std::fputs("provider reorder was rejected\n", stderr);
        return 1;
    }
    snapshot = store.snapshot();
    const auto reorder_changes = store.changes_after(before_reorder);
    if (snapshot->providers[0]->identity.application_id != "dev.example.second"
        || reorder_changes.history_gap || reorder_changes.changes.size() != 1
        || reorder_changes.changes[0].kind != SceneChangeKind::reset) {
        std::fputs("provider reorder was not published as an atomic reset\n", stderr);
        return 1;
    }

    const auto before_hide = snapshot->revision;
    store.set_application_policy(
        "dev.example.second", ApplicationPolicyUpdate { true, false, -10 });
    snapshot = store.snapshot();
    if (snapshot->providers.size() != 1
        || snapshot->providers[0]->identity.application_id != "dev.example.first"
        || store.changes_after(before_hide).changes[0].kind != SceneChangeKind::reset) {
        std::fputs("hidden provider remained in the renderer snapshot\n", stderr);
        return 1;
    }

    const auto hidden_revision = snapshot->revision;
    store.disconnect(2);
    if (store.snapshot()->revision != hidden_revision
        || !store.changes_after(hidden_revision).changes.empty()) {
        std::fputs("hidden provider disconnect produced renderer traffic\n", stderr);
        return 1;
    }

    store.set_enabled(false);
    snapshot = store.snapshot();
    if (!snapshot->providers.empty()) {
        std::fputs("global disable did not hide every provider\n", stderr);
        return 1;
    }
    store.set_enabled(true);
    if (store.snapshot()->providers.size() != 1) {
        std::fputs("global enable did not restore approved visible providers\n", stderr);
        return 1;
    }

    bool candidate_was_disabled = false;
    const auto before_failed_write = store.snapshot()->revision;
    if (store.set_enabled(false, [&](const auto& candidate) {
            candidate_was_disabled = !candidate.enabled;
            return false;
        })
        || !candidate_was_disabled || !store.policy().enabled
        || store.snapshot()->revision != before_failed_write) {
        std::fputs("failed policy persistence changed the live scene\n", stderr);
        return 1;
    }

    store.set_require_approval(true);
    const auto before_pending = store.snapshot()->revision;
    store.register_provider(3, provider("dev.example.pending", "one", "Pending"));
    if (store.snapshot()->providers.size() != 1
        || store.snapshot()->revision != before_pending) {
        std::fputs("new provider bypassed approval or changed renderer state\n", stderr);
        return 1;
    }
    const auto pending_policy = store.policy();
    if (pending_policy.applications.size() != 3
        || pending_policy.applications[2].application_id != "dev.example.pending"
        || pending_policy.applications[2].approved
        || pending_policy.applications[2].active_instances != 1) {
        std::fputs("pending provider was not exposed to the controller\n", stderr);
        return 1;
    }

    if (!store.set_application_policy(
            "dev.example.pending", ApplicationPolicyUpdate { true, true, 20 })
        || store.snapshot()->providers.size() != 2) {
        std::fputs("approved provider did not become visible\n", stderr);
        return 1;
    }

    if (!store.set_application_position("dev.example.pending", 0)
        || store.snapshot()->providers[0]->identity.application_id
            != "dev.example.pending"
        || store.policy().applications[0].order != 0) {
        std::fputs("provider position was not atomically normalized\n", stderr);
        return 1;
    }
    return 0;
}
