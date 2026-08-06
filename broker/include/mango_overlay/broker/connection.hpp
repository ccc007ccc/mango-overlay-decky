#pragma once

#include "mango_overlay/broker/handshake_processor.hpp"
#include "mango_overlay/protocol/handshake.hpp"

#include <chrono>
#include <optional>

namespace mango_overlay::broker {

class ProviderSession;
class RendererSession;
class ControllerSession;

enum class ConnectionResult {
    accepted,
    rejected,
    peer_closed,
    receive_failed,
    send_failed,
};

struct HandshakeConnectionResult {
    ConnectionResult result;
    std::optional<AcceptedSession> accepted;
};

bool configure_connection_socket(
    int socket_fd,
    std::chrono::milliseconds initial_receive_timeout,
    std::chrono::milliseconds send_timeout);

bool clear_receive_timeout(int socket_fd);

HandshakeConnectionResult serve_initial_handshake(
    int socket_fd,
    const protocol::ServerHandshake& server);

ConnectionResult serve_provider_session(int socket_fd, ProviderSession& session);
ConnectionResult serve_renderer_session(int socket_fd, RendererSession& session);
ConnectionResult serve_controller_session(int socket_fd, ControllerSession& session);

} // namespace mango_overlay::broker
