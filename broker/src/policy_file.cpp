#include "mango_overlay/broker/policy_file.hpp"
#include "mango_overlay/scene/validation.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <unordered_set>

namespace mango_overlay::broker {

namespace {

constexpr std::size_t maximum_policy_bytes = 256 * 1024;
constexpr std::size_t maximum_policy_applications = 256;
std::atomic<std::uint64_t> temporary_sequence { 1 };

bool valid_application(const scene::ApplicationPolicy& application)
{
    return scene::valid_provider_identity(scene::ProviderIdentity {
        application.application_id,
        "policy",
        application.display_name,
        1,
        1,
        scene::Visibility::always,
    });
}

bool write_all(int descriptor, const std::vector<std::uint8_t>& contents)
{
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto result = write(
            descriptor,
            contents.data() + written,
            contents.size() - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

} // namespace

std::vector<std::uint8_t> encode_policy_file(const scene::OverlayPolicy& policy)
{
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<MangoOverlay::Wire::PersistedApplicationPolicy>>
        applications;
    applications.reserve(policy.applications.size());
    for (const auto& application : policy.applications) {
        applications.push_back(
            MangoOverlay::Wire::CreatePersistedApplicationPolicy(
                builder,
                builder.CreateString(application.application_id),
                builder.CreateString(application.display_name),
                application.approved,
                application.visible,
                application.order));
    }
    const auto document = MangoOverlay::Wire::CreatePolicyDocument(
        builder,
        1,
        policy.enabled,
        policy.require_approval,
        builder.CreateVector(applications));
    builder.Finish(document);
    return { builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() };
}

PolicyLoadResult decode_policy_file(protocol::ByteView contents)
{
    if (contents.data == nullptr || contents.size == 0
        || contents.size > maximum_policy_bytes) {
        return PolicyFileError::invalid_data;
    }
    flatbuffers::Verifier verifier(contents.data, contents.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::PolicyDocument>(nullptr)) {
        return PolicyFileError::invalid_data;
    }
    const auto* document
        = flatbuffers::GetRoot<MangoOverlay::Wire::PolicyDocument>(contents.data);
    if (document->schema() != 1 || document->applications() == nullptr
        || document->applications()->size() > maximum_policy_applications) {
        return PolicyFileError::invalid_data;
    }

    scene::OverlayPolicy policy;
    policy.enabled = document->enabled();
    policy.require_approval = document->require_approval();
    policy.applications.reserve(document->applications()->size());
    std::unordered_set<std::string> identifiers;
    for (const auto* wire : *document->applications()) {
        if (wire == nullptr || wire->application_id() == nullptr
            || wire->display_name() == nullptr) {
            return PolicyFileError::invalid_data;
        }
        scene::ApplicationPolicy application {
            wire->application_id()->str(),
            wire->display_name()->str(),
            wire->approved(),
            wire->visible(),
            wire->order(),
            0,
        };
        if (!valid_application(application)
            || !identifiers.emplace(application.application_id).second) {
            return PolicyFileError::invalid_data;
        }
        policy.applications.push_back(std::move(application));
    }
    return policy;
}

PolicyLoadResult load_policy_file(const std::filesystem::path& path)
{
    struct stat path_info {};
    if (lstat(path.c_str(), &path_info) != 0) {
        return errno == ENOENT
            ? PolicyLoadResult(scene::OverlayPolicy {})
            : PolicyLoadResult(PolicyFileError::io_error);
    }
    if (!S_ISREG(path_info.st_mode) || path_info.st_uid != geteuid()
        || (path_info.st_mode & 0022) != 0 || path_info.st_size <= 0
        || static_cast<std::uint64_t>(path_info.st_size) > maximum_policy_bytes) {
        return PolicyFileError::unsafe_path;
    }

    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return PolicyFileError::io_error;
    }
    struct stat open_info {};
    if (fstat(descriptor, &open_info) != 0
        || open_info.st_dev != path_info.st_dev || open_info.st_ino != path_info.st_ino) {
        close(descriptor);
        return PolicyFileError::unsafe_path;
    }

    std::vector<std::uint8_t> contents(static_cast<std::size_t>(open_info.st_size));
    std::size_t read_bytes = 0;
    while (read_bytes < contents.size()) {
        const auto result = read(
            descriptor,
            contents.data() + read_bytes,
            contents.size() - read_bytes);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            close(descriptor);
            return PolicyFileError::io_error;
        }
        read_bytes += static_cast<std::size_t>(result);
    }
    close(descriptor);
    return decode_policy_file(
        protocol::ByteView { contents.data(), contents.size() });
}

PolicyFileError save_policy_file(
    const std::filesystem::path& path,
    const scene::OverlayPolicy& policy)
{
    const auto contents = encode_policy_file(policy);
    if (contents.empty() || contents.size() > maximum_policy_bytes) {
        return PolicyFileError::invalid_data;
    }
    for (const auto& application : policy.applications) {
        if (!valid_application(application)) {
            return PolicyFileError::invalid_data;
        }
    }

    const auto parent = path.parent_path();
    std::error_code filesystem_error;
    if (!std::filesystem::exists(parent, filesystem_error)) {
        if (!std::filesystem::create_directory(parent, filesystem_error)) {
            return PolicyFileError::io_error;
        }
        if (chmod(parent.c_str(), 0700) != 0) {
            return PolicyFileError::io_error;
        }
    }
    struct stat parent_info {};
    if (lstat(parent.c_str(), &parent_info) != 0
        || !S_ISDIR(parent_info.st_mode) || parent_info.st_uid != geteuid()
        || (parent_info.st_mode & 0022) != 0) {
        return PolicyFileError::unsafe_path;
    }

    struct stat destination_info {};
    if (lstat(path.c_str(), &destination_info) == 0) {
        if (!S_ISREG(destination_info.st_mode)
            || destination_info.st_uid != geteuid()
            || (destination_info.st_mode & 0022) != 0) {
            return PolicyFileError::unsafe_path;
        }
    } else if (errno != ENOENT) {
        return PolicyFileError::io_error;
    }

    const auto temporary = parent
        / (".policy-" + std::to_string(getpid()) + "-"
            + std::to_string(temporary_sequence.fetch_add(1)) + ".tmp");
    const int descriptor = open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (descriptor < 0) {
        return PolicyFileError::io_error;
    }
    const bool written = write_all(descriptor, contents)
        && fchmod(descriptor, 0600) == 0 && fsync(descriptor) == 0;
    const bool closed = close(descriptor) == 0;
    if (!written || !closed) {
        unlink(temporary.c_str());
        return PolicyFileError::io_error;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return PolicyFileError::io_error;
    }
    const int parent_descriptor = open(
        parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent_descriptor < 0) {
        return PolicyFileError::io_error;
    }
    const bool synchronized = fsync(parent_descriptor) == 0;
    close(parent_descriptor);
    return synchronized ? PolicyFileError::none : PolicyFileError::io_error;
}

} // namespace mango_overlay::broker
