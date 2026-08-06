#include "mango_overlay/protocol/controller.hpp"
#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/verifier.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;

void json_string(const std::string& value)
{
    std::putchar('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            std::putchar('\\');
            std::putchar(character);
        } else if (character < 0x20) {
            std::fputs("\\u00", stdout);
            std::putchar(hex[character >> 4U]);
            std::putchar(hex[character & 0x0fU]);
        } else {
            std::putchar(character);
        }
    }
    std::putchar('"');
}

void print_status(const mango_overlay::protocol::ControllerStatus& status)
{
    std::printf(
        "{\"enabled\":%s,\"require_approval\":%s,\"scene_revision\":%llu,"
        "\"applications\":[",
        status.enabled ? "true" : "false",
        status.require_approval ? "true" : "false",
        static_cast<unsigned long long>(status.scene_revision));
    for (std::size_t index = 0; index < status.applications.size(); ++index) {
        if (index != 0) {
            std::putchar(',');
        }
        const auto& application = status.applications[index];
        std::fputs("{\"application_id\":", stdout);
        json_string(application.application_id);
        std::fputs(",\"display_name\":", stdout);
        json_string(application.display_name);
        std::printf(
            ",\"approved\":%s,\"visible\":%s,\"order\":%d,"
            "\"active_instances\":%u}",
            application.approved ? "true" : "false",
            application.visible ? "true" : "false",
            application.order,
            application.active_instances);
    }
    std::fputs("]}\n", stdout);
}

std::optional<bool> parse_bool(std::string_view value)
{
    if (value == "true" || value == "on" || value == "1") {
        return true;
    }
    if (value == "false" || value == "off" || value == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::int32_t> parse_order(std::string_view value)
{
    std::int32_t order = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), order);
    if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return order;
}

int connect_socket(const std::string& path)
{
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
        return -1;
    }
    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return -1;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            address_size)
        != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

std::optional<std::vector<std::uint8_t>> exchange(
    int descriptor,
    const std::vector<std::uint8_t>& request)
{
    if (mango_overlay::protocol::send_seqpacket(
            descriptor, ByteView { request.data(), request.size() })
        != mango_overlay::protocol::SeqpacketResult::accepted) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> response(
        mango_overlay::protocol::packet_header_size
        + mango_overlay::protocol::maximum_payload_size);
    std::size_t received = 0;
    mango_overlay::protocol::UniqueFileDescriptor attachment;
    if (mango_overlay::protocol::receive_seqpacket(
            descriptor,
            response.data(),
            response.size(),
            received,
            attachment)
            != mango_overlay::protocol::SeqpacketResult::accepted
        || attachment) {
        return std::nullopt;
    }
    response.resize(received);
    return response;
}

const DecodedPacketView* decode_response(
    const std::vector<std::uint8_t>& response,
    mango_overlay::protocol::DecodeResult& storage)
{
    storage = mango_overlay::protocol::decode_packet(
        ByteView { response.data(), response.size() });
    return std::get_if<DecodedPacketView>(&storage);
}

void print_protocol_error(const DecodedPacketView& packet)
{
    flatbuffers::Verifier verifier(packet.payload.data, packet.payload.size);
    if (verifier.VerifyBuffer<MangoOverlay::Wire::ErrorResponse>(nullptr)) {
        const auto* error = flatbuffers::GetRoot<MangoOverlay::Wire::ErrorResponse>(
            packet.payload.data);
        if (error->message() != nullptr) {
            std::fprintf(stderr, "%s\n", error->message()->c_str());
            return;
        }
    }
    std::fputs("mango-overlayd rejected the request\n", stderr);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2
        && std::string_view(argv[1]) == "--mango-overlay-self-test") {
        std::puts("mango-overlayctl version=" MANGO_OVERLAY_VERSION " protocol=1.0 status=ok");
        return 0;
    }

    std::string socket_path;
    int argument = 1;
    if (argument + 1 < argc && std::string_view(argv[argument]) == "--socket") {
        socket_path = argv[argument + 1];
        argument += 2;
    } else {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (runtime == nullptr || *runtime == '\0') {
            std::fputs("XDG_RUNTIME_DIR is unavailable\n", stderr);
            return 69;
        }
        socket_path = std::string(runtime) + "/mango-overlay-decky.sock";
    }
    if (argument >= argc) {
        std::fputs("usage: mango-overlayctl [--socket PATH] COMMAND\n", stderr);
        return 64;
    }

    MessageType message_type = MessageType::controller_get_status;
    std::vector<std::uint8_t> payload;
    const std::string_view command(argv[argument++]);
    if (command == "status" && argument == argc) {
        payload = mango_overlay::protocol::encode_controller_request(
            mango_overlay::protocol::ControllerGetStatus {});
    } else if (command == "set-enabled" && argument + 1 == argc) {
        const auto value = parse_bool(argv[argument]);
        if (!value.has_value()) {
            return 64;
        }
        message_type = MessageType::controller_set_enabled;
        payload = mango_overlay::protocol::encode_controller_request(
            mango_overlay::protocol::ControllerSetEnabled { *value });
    } else if (command == "set-require-approval" && argument + 1 == argc) {
        const auto value = parse_bool(argv[argument]);
        if (!value.has_value()) {
            return 64;
        }
        message_type = MessageType::controller_set_require_approval;
        payload = mango_overlay::protocol::encode_controller_request(
            mango_overlay::protocol::ControllerSetRequireApproval { *value });
    } else if (command == "set-provider" && argument + 4 == argc) {
        const std::string application_id(argv[argument]);
        const auto approved = parse_bool(argv[argument + 1]);
        const auto visible = parse_bool(argv[argument + 2]);
        const auto order = parse_order(argv[argument + 3]);
        if (!approved.has_value() || !visible.has_value() || !order.has_value()) {
            return 64;
        }
        message_type = MessageType::controller_set_application_policy;
        payload = mango_overlay::protocol::encode_controller_request(
            mango_overlay::protocol::ControllerSetApplicationPolicy {
                application_id, *approved, *visible, *order });
    } else if (command == "set-provider-position" && argument + 2 == argc) {
        const std::string application_id(argv[argument]);
        const auto position = parse_order(argv[argument + 1]);
        if (!position.has_value() || *position < 0) {
            return 64;
        }
        message_type = MessageType::controller_set_application_position;
        payload = mango_overlay::protocol::encode_controller_request(
            mango_overlay::protocol::ControllerSetApplicationPosition {
                application_id, static_cast<std::uint32_t>(*position) });
    } else {
        std::fputs("invalid mango-overlayctl command\n", stderr);
        return 64;
    }

    const int descriptor = connect_socket(socket_path);
    if (descriptor < 0) {
        std::fprintf(stderr, "could not connect to %s\n", socket_path.c_str());
        return 69;
    }
    const auto hello_payload = mango_overlay::protocol::encode_hello(
        mango_overlay::protocol::Hello {
            mango_overlay::protocol::ConnectionRole::controller,
            mango_overlay::protocol::ProtocolVersion { 1, 0 },
            mango_overlay::protocol::ProtocolVersion { 1, 0 },
            mango_overlay::protocol::capability::controller_policy,
            0,
            "mango-overlayctl/" MANGO_OVERLAY_VERSION,
        });
    const auto hello = mango_overlay::protocol::encode_packet(
        mango_overlay::protocol::PacketHeader {
            mango_overlay::protocol::ProtocolVersion { 1, 0 },
            MessageType::hello,
            0,
            1,
        },
        ByteView { hello_payload.data(), hello_payload.size() });
    auto response = exchange(descriptor, hello);
    mango_overlay::protocol::DecodeResult decoded;
    const auto* packet = response.has_value()
        ? decode_response(*response, decoded)
        : nullptr;
    if (packet == nullptr || packet->header.message_type != MessageType::hello_accepted) {
        close(descriptor);
        std::fputs("controller handshake failed\n", stderr);
        return 76;
    }

    const auto request = mango_overlay::protocol::encode_packet(
        mango_overlay::protocol::PacketHeader {
            mango_overlay::protocol::ProtocolVersion { 1, 0 },
            message_type,
            0,
            2,
        },
        ByteView { payload.data(), payload.size() });
    response = exchange(descriptor, request);
    close(descriptor);
    packet = response.has_value() ? decode_response(*response, decoded) : nullptr;
    if (packet == nullptr) {
        std::fputs("controller response failed\n", stderr);
        return 74;
    }
    if (packet->header.message_type == MessageType::error) {
        print_protocol_error(*packet);
        return 1;
    }
    if (packet->header.message_type != MessageType::controller_status) {
        std::fputs("controller returned an unexpected response\n", stderr);
        return 76;
    }
    const auto status = mango_overlay::protocol::decode_controller_status(packet->payload);
    const auto* value
        = std::get_if<mango_overlay::protocol::ControllerStatus>(&status);
    if (value == nullptr) {
        std::fputs("controller returned malformed status\n", stderr);
        return 76;
    }
    print_status(*value);
    return 0;
}
