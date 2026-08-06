#include "mango_overlay/broker/connection.hpp"
#include "mango_overlay/broker/controller_session.hpp"
#include "mango_overlay/broker/policy_file.hpp"
#include "mango_overlay/broker/provider_session.hpp"
#include "mango_overlay/broker/renderer_session.hpp"
#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/scene/store.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>

namespace {

std::optional<int> parse_nonnegative_int(std::string_view value)
{
    int result = -1;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || result < 0) {
        return std::nullopt;
    }
    return result;
}

std::optional<int> systemd_socket()
{
    const char* listen_pid = std::getenv("LISTEN_PID");
    const char* listen_fds = std::getenv("LISTEN_FDS");
    if (listen_pid == nullptr || listen_fds == nullptr) {
        return std::nullopt;
    }

    const auto pid = parse_nonnegative_int(listen_pid);
    const auto count = parse_nonnegative_int(listen_fds);
    if (!pid.has_value() || !count.has_value()
        || *pid != static_cast<int>(getpid()) || *count != 1) {
        return std::nullopt;
    }
    return 3;
}

bool valid_listener(int descriptor)
{
    int socket_type = 0;
    socklen_t option_size = sizeof(socket_type);
    if (getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type, &option_size) != 0
        || socket_type != SOCK_SEQPACKET) {
        return false;
    }

    int accepting = 0;
    option_size = sizeof(accepting);
    return getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &option_size) == 0
        && accepting == 1;
}

bool peer_is_current_user(int descriptor)
{
    ucred credentials {};
    socklen_t credentials_size = sizeof(credentials);
    return getsockopt(
               descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size)
            == 0
        && credentials.uid == geteuid();
}

mango_overlay::broker::ConnectionResult serve_connection(
    int descriptor,
    const mango_overlay::protocol::ServerHandshake& server,
    mango_overlay::scene::SceneStore& scenes,
    mango_overlay::scene::ConnectionId connection_id,
    const mango_overlay::scene::PolicyCommit& persist_policy)
{
    using mango_overlay::broker::ConnectionResult;

    if (!peer_is_current_user(descriptor)) {
        return ConnectionResult::rejected;
    }

    const auto handshake = mango_overlay::broker::serve_initial_handshake(descriptor, server);
    if (handshake.result != ConnectionResult::accepted
        || !handshake.accepted.has_value()) {
        return handshake.result == ConnectionResult::accepted
            ? ConnectionResult::rejected
            : handshake.result;
    }

    ConnectionResult result = ConnectionResult::rejected;
    if (handshake.accepted->role == mango_overlay::protocol::ConnectionRole::provider) {
        mango_overlay::broker::ProviderSession session(
            scenes,
            connection_id,
            handshake.accepted->version);
        result = mango_overlay::broker::serve_provider_session(descriptor, session);
    } else if (
        handshake.accepted->role == mango_overlay::protocol::ConnectionRole::renderer) {
        mango_overlay::broker::RendererSession session(
            scenes, handshake.accepted->version);
        result = mango_overlay::broker::serve_renderer_session(descriptor, session);
    } else if (
        handshake.accepted->role == mango_overlay::protocol::ConnectionRole::controller) {
        mango_overlay::broker::ControllerSession session(
            scenes, handshake.accepted->version, persist_policy);
        result = mango_overlay::broker::serve_controller_session(descriptor, session);
    }
    return result == ConnectionResult::peer_closed ? ConnectionResult::accepted : result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2
        && std::string_view(argv[1]) == "--mango-overlay-self-test") {
        std::puts("mango-overlayd version=" MANGO_OVERLAY_VERSION " protocol=1.0 status=ok");
        return 0;
    }

    std::optional<int> listener;
    std::optional<std::filesystem::path> policy_file;
    bool once = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--once") {
            once = true;
            continue;
        }
        if (argument == "--listen-fd" && index + 1 < argc) {
            listener = parse_nonnegative_int(argv[++index]);
            continue;
        }
        if (argument == "--policy-file" && index + 1 < argc) {
            policy_file = std::filesystem::path(argv[++index]);
            if (!policy_file->is_absolute()) {
                std::fputs("Policy file path must be absolute\n", stderr);
                return 64;
            }
            continue;
        }
        std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[index]);
        return 64;
    }

    if (!listener.has_value()) {
        listener = systemd_socket();
    }
    if (!listener.has_value() || !valid_listener(*listener)) {
        std::fputs("mango-overlayd requires one SOCK_SEQPACKET listening socket\n", stderr);
        return 69;
    }

    const mango_overlay::protocol::ServerHandshake server {
        mango_overlay::protocol::ProtocolVersion { 1, 0 },
        mango_overlay::protocol::ProtocolVersion { 1, 0 },
        mango_overlay::protocol::capability::retained_scene_transactions
            | mango_overlay::protocol::capability::renderer_scene_stream
            | mango_overlay::protocol::capability::image_resources
            | mango_overlay::protocol::capability::controller_policy,
        "mango-overlayd/" MANGO_OVERLAY_VERSION,
    };
    mango_overlay::scene::OverlayPolicy initial_policy;
    if (policy_file.has_value()) {
        const auto loaded = mango_overlay::broker::load_policy_file(*policy_file);
        const auto* policy = std::get_if<mango_overlay::scene::OverlayPolicy>(&loaded);
        if (policy == nullptr) {
            std::fputs("mango-overlayd could not load its policy file\n", stderr);
            return 78;
        }
        initial_policy = *policy;
    }
    bool has_test_provider = false;
    for (const auto& application : initial_policy.applications) {
        if (application.application_id == "dev.mango-overlay.test") {
            has_test_provider = true;
            break;
        }
    }
    if (!has_test_provider && initial_policy.applications.size() < 256) {
        initial_policy.applications.push_back(
            mango_overlay::scene::ApplicationPolicy {
                "dev.mango-overlay.test",
                "Test Canvas",
                true,
                true,
                static_cast<std::int32_t>(initial_policy.applications.size()),
                0,
            });
    }
    mango_overlay::scene::SceneStore scenes({}, std::move(initial_policy));
    mango_overlay::scene::PolicyCommit persist_policy;
    if (policy_file.has_value()) {
        persist_policy = [path = *policy_file](
                             const mango_overlay::scene::OverlayPolicy& policy) {
            return mango_overlay::broker::save_policy_file(path, policy)
                == mango_overlay::broker::PolicyFileError::none;
        };
    }
    std::atomic<mango_overlay::scene::ConnectionId> next_connection { 1 };
    std::atomic<unsigned int> active_connections { 0 };
    constexpr unsigned int maximum_connections = 64;

    do {
        int connection;
        do {
            connection = accept4(*listener, nullptr, nullptr, SOCK_CLOEXEC);
        } while (connection < 0 && errno == EINTR);
        if (connection < 0) {
            std::fprintf(stderr, "accept failed: %s\n", std::strerror(errno));
            return 74;
        }

        if (!mango_overlay::broker::configure_connection_socket(
                connection,
                std::chrono::seconds(2),
                std::chrono::seconds(2))) {
            close(connection);
            continue;
        }

        if (once) {
            const auto result = serve_connection(
                connection,
                server,
                scenes,
                next_connection.fetch_add(1, std::memory_order_relaxed),
                persist_policy);
            close(connection);
            return result == mango_overlay::broker::ConnectionResult::accepted ? 0 : 1;
        }

        if (active_connections.load(std::memory_order_relaxed) >= maximum_connections) {
            close(connection);
            continue;
        }

        const auto connection_id = next_connection.fetch_add(1, std::memory_order_relaxed);
        active_connections.fetch_add(1, std::memory_order_relaxed);
        try {
            std::thread([
                            connection,
                            connection_id,
                            &server,
                            &scenes,
                            &persist_policy,
                            &active_connections]() {
                try {
                    serve_connection(
                        connection,
                        server,
                        scenes,
                        connection_id,
                        persist_policy);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "connection failed: %s\n", error.what());
                } catch (...) {
                    std::fputs("connection failed with an unknown error\n", stderr);
                }
                close(connection);
                active_connections.fetch_sub(1, std::memory_order_relaxed);
            }).detach();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "could not start connection worker: %s\n", error.what());
            active_connections.fetch_sub(1, std::memory_order_relaxed);
            close(connection);
        }
    } while (true);
}
