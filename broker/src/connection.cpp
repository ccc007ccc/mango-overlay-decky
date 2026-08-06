#include "mango_overlay/broker/connection.hpp"
#include "mango_overlay/broker/controller_session.hpp"
#include "mango_overlay/broker/handshake_processor.hpp"
#include "mango_overlay/broker/provider_session.hpp"
#include "mango_overlay/broker/renderer_session.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <variant>

namespace mango_overlay::broker {

namespace {

bool set_socket_timeout(
    int socket_fd,
    int option,
    std::chrono::milliseconds timeout)
{
    if (timeout.count() < 0) {
        return false;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto remainder = std::chrono::duration_cast<std::chrono::microseconds>(
        timeout - seconds);
    const timeval value {
        static_cast<time_t>(seconds.count()),
        static_cast<suseconds_t>(remainder.count()),
    };
    return setsockopt(socket_fd, SOL_SOCKET, option, &value, sizeof(value)) == 0;
}

ConnectionResult send_packet(int socket_fd, protocol::ByteView packet)
{
    return protocol::send_seqpacket(socket_fd, packet) == protocol::SeqpacketResult::accepted
        ? ConnectionResult::accepted
        : ConnectionResult::send_failed;
}

ConnectionResult receive_packet(
    int socket_fd,
    std::vector<std::uint8_t>& buffer,
    std::size_t& received_size,
    protocol::UniqueFileDescriptor& attachment)
{
    const auto result = protocol::receive_seqpacket(
        socket_fd,
        buffer.data(),
        buffer.size(),
        received_size,
        attachment);
    if (result == protocol::SeqpacketResult::peer_closed) {
        return ConnectionResult::peer_closed;
    }
    if (result == protocol::SeqpacketResult::io_error) {
        return ConnectionResult::receive_failed;
    }
    if (result != protocol::SeqpacketResult::accepted) {
        return ConnectionResult::rejected;
    }
    return ConnectionResult::accepted;
}

ConnectionResult send_renderer_response(
    int socket_fd,
    const RendererSessionResponse& response)
{
    for (const auto& packet : response.packets) {
        protocol::UniqueFileDescriptor descriptor;
        if (packet.attachment != nullptr) {
            descriptor = protocol::make_sealed_memfd(protocol::ByteView {
                packet.attachment->encoded.data(),
                packet.attachment->encoded.size(),
            });
            if (!descriptor) {
                return ConnectionResult::send_failed;
            }
        }
        const auto result = protocol::send_seqpacket(
            socket_fd,
            protocol::ByteView { packet.bytes.data(), packet.bytes.size() },
            descriptor.get());
        if (result != protocol::SeqpacketResult::accepted) {
            return ConnectionResult::send_failed;
        }
    }
    return response.close_after_send
        ? ConnectionResult::rejected
        : ConnectionResult::accepted;
}

} // namespace

bool configure_connection_socket(
    int socket_fd,
    std::chrono::milliseconds initial_receive_timeout,
    std::chrono::milliseconds send_timeout)
{
    return set_socket_timeout(socket_fd, SO_RCVTIMEO, initial_receive_timeout)
        && set_socket_timeout(socket_fd, SO_SNDTIMEO, send_timeout);
}

bool clear_receive_timeout(int socket_fd)
{
    return set_socket_timeout(socket_fd, SO_RCVTIMEO, std::chrono::milliseconds(0));
}

HandshakeConnectionResult serve_initial_handshake(
    int socket_fd,
    const protocol::ServerHandshake& server)
{
    constexpr std::size_t maximum_initial_packet_size = 4096;
    std::vector<std::uint8_t> request(maximum_initial_packet_size);

    std::size_t received = 0;
    protocol::UniqueFileDescriptor attachment;
    const auto receive_result = receive_packet(
        socket_fd, request, received, attachment);
    if (receive_result != ConnectionResult::accepted) {
        return { receive_result, std::nullopt };
    }

    auto outcome = process_initial_packet(
        protocol::ByteView { request.data(), received }, server);
    if (outcome.response.empty()) {
        return { ConnectionResult::rejected, std::nullopt };
    }

    const auto decoded_response = protocol::decode_packet(
        protocol::ByteView { outcome.response.data(), outcome.response.size() });
    const auto* response_packet = std::get_if<protocol::DecodedPacketView>(&decoded_response);
    if (response_packet == nullptr) {
        return { ConnectionResult::send_failed, std::nullopt };
    }

    const auto send_result = send_packet(
        socket_fd,
        protocol::ByteView { outcome.response.data(), outcome.response.size() });
    if (send_result != ConnectionResult::accepted) {
        return { send_result, std::nullopt };
    }

    if (response_packet->header.message_type != protocol::MessageType::hello_accepted
        || !outcome.accepted.has_value()) {
        return { ConnectionResult::rejected, std::nullopt };
    }
    return { ConnectionResult::accepted, std::move(outcome.accepted) };
}

ConnectionResult serve_provider_session(int socket_fd, ProviderSession& session)
{
    std::vector<std::uint8_t> request(
        protocol::packet_header_size + protocol::maximum_payload_size);

    bool receive_timeout_active = true;
    while (true) {
        std::size_t received = 0;
        protocol::UniqueFileDescriptor attachment;
        const auto receive_result = receive_packet(
            socket_fd, request, received, attachment);
        if (receive_result != ConnectionResult::accepted) {
            return receive_result;
        }

        const auto response = session.process(
            protocol::ByteView { request.data(), received }, attachment.get());
        if (!response.packet.empty()) {
            const auto result = send_packet(
                socket_fd,
                protocol::ByteView { response.packet.data(), response.packet.size() });
            if (result != ConnectionResult::accepted) {
                return result;
            }
        }
        if (response.close_after_send) {
            return ConnectionResult::rejected;
        }
        if (receive_timeout_active && session.registered()) {
            if (!clear_receive_timeout(socket_fd)) {
                return ConnectionResult::receive_failed;
            }
            receive_timeout_active = false;
        }
    }
}

ConnectionResult serve_renderer_session(int socket_fd, RendererSession& session)
{
    std::vector<std::uint8_t> request(
        protocol::packet_header_size + protocol::maximum_payload_size);

    std::size_t received = 0;
    protocol::UniqueFileDescriptor attachment;
    auto result = receive_packet(socket_fd, request, received, attachment);
    if (result != ConnectionResult::accepted) {
        return result;
    }
    result = send_renderer_response(
        socket_fd,
        session.process(protocol::ByteView { request.data(), received }));
    if (result != ConnectionResult::accepted) {
        return result;
    }
    if (!clear_receive_timeout(socket_fd)) {
        return ConnectionResult::receive_failed;
    }

    while (true) {
        pollfd socket_poll { socket_fd, POLLIN, 0 };
        int poll_result;
        do {
            poll_result = poll(&socket_poll, 1, 0);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            return ConnectionResult::receive_failed;
        }
        if ((socket_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return ConnectionResult::peer_closed;
        }
        if ((socket_poll.revents & POLLIN) != 0) {
            received = 0;
            result = receive_packet(socket_fd, request, received, attachment);
            if (result != ConnectionResult::accepted) {
                return result;
            }
            result = send_renderer_response(
                socket_fd,
                session.process(protocol::ByteView { request.data(), received }));
            if (result != ConnectionResult::accepted) {
                return result;
            }
        }

        result = send_renderer_response(
            socket_fd,
            session.wait_for_updates(std::chrono::milliseconds(100)));
        if (result != ConnectionResult::accepted) {
            return result;
        }
    }
}

ConnectionResult serve_controller_session(int socket_fd, ControllerSession& session)
{
    std::vector<std::uint8_t> request(
        protocol::packet_header_size + protocol::maximum_payload_size);
    if (!clear_receive_timeout(socket_fd)) {
        return ConnectionResult::receive_failed;
    }

    while (true) {
        std::size_t received = 0;
        protocol::UniqueFileDescriptor attachment;
        const auto receive_result = receive_packet(
            socket_fd, request, received, attachment);
        if (receive_result != ConnectionResult::accepted) {
            return receive_result;
        }
        if (attachment) {
            return ConnectionResult::rejected;
        }
        const auto response = session.process(
            protocol::ByteView { request.data(), received });
        const auto send_result = send_packet(
            socket_fd,
            protocol::ByteView { response.packet.data(), response.packet.size() });
        if (send_result != ConnectionResult::accepted) {
            return send_result;
        }
        if (response.close_after_send) {
            return ConnectionResult::rejected;
        }
    }
}

} // namespace mango_overlay::broker
