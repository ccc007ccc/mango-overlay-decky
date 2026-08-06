#include "mango_overlay/broker/policy_file.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using mango_overlay::broker::PolicyFileError;
using mango_overlay::broker::load_policy_file;
using mango_overlay::broker::save_policy_file;
using mango_overlay::scene::ApplicationPolicy;
using mango_overlay::scene::OverlayPolicy;

int main()
{
    char template_path[] = "/tmp/mango-overlay-policy-XXXXXX";
    const char* directory = mkdtemp(template_path);
    if (directory == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    const std::filesystem::path root(directory);
    const auto path = root / "config/policy.fb";

    const auto missing = load_policy_file(path);
    const auto* defaults = std::get_if<OverlayPolicy>(&missing);
    if (defaults == nullptr || !defaults->enabled || defaults->require_approval
        || !defaults->applications.empty()) {
        std::fputs("missing policy did not produce safe defaults\n", stderr);
        return 1;
    }

    const OverlayPolicy expected {
        false,
        true,
        {
            ApplicationPolicy {
                "dev.example.provider", "Example", true, false, -4, 99 },
        },
    };
    if (save_policy_file(path, expected) != PolicyFileError::none) {
        std::fputs("valid policy could not be saved\n", stderr);
        return 1;
    }
    const auto loaded = load_policy_file(path);
    const auto* policy = std::get_if<OverlayPolicy>(&loaded);
    struct stat info {};
    if (policy == nullptr || policy->enabled || !policy->require_approval
        || policy->applications.size() != 1
        || policy->applications[0].application_id != "dev.example.provider"
        || policy->applications[0].display_name != "Example"
        || !policy->applications[0].approved
        || policy->applications[0].visible
        || policy->applications[0].order != -4
        || policy->applications[0].active_instances != 0
        || stat(path.c_str(), &info) != 0 || (info.st_mode & 0777) != 0600) {
        std::fputs("saved policy did not round-trip securely\n", stderr);
        return 1;
    }

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "not a flatbuffer";
    }
    if (!std::holds_alternative<PolicyFileError>(load_policy_file(path))) {
        std::fputs("corrupt policy was accepted\n", stderr);
        return 1;
    }

    std::filesystem::remove(path);
    const auto external = root / "external";
    {
        std::ofstream output(external);
        output << "keep";
    }
    std::filesystem::create_symlink(external, path);
    if (save_policy_file(path, expected) != PolicyFileError::unsafe_path) {
        std::fputs("policy writer accepted a symbolic-link destination\n", stderr);
        return 1;
    }
    std::ifstream external_input(external);
    std::string external_contents;
    external_input >> external_contents;
    if (external_contents != "keep") {
        std::fputs("policy writer changed the symbolic-link target\n", stderr);
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
