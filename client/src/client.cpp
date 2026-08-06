#include "mango_overlay/client.h"

#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay/scene/validation.hpp"
#include "mango_overlay/wire/scene_codec.hpp"
#include "mango_overlay_generated.h"

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using mango_overlay::protocol::ByteView;
using mango_overlay::protocol::DecodedPacketView;
using mango_overlay::protocol::MessageType;
using mango_overlay::protocol::PacketHeader;
using mango_overlay::protocol::ProtocolVersion;

constexpr ProtocolVersion protocol_version { 1, 0 };
constexpr std::size_t maximum_string_bytes = 128;
constexpr std::size_t maximum_text_bytes = 4096;
constexpr std::size_t inline_resource_threshold = 256 * 1024;
constexpr std::uint32_t default_timeout_ms = 2000;
constexpr std::uint32_t maximum_timeout_ms = 60000;
const mango_overlay::scene::SceneLimits scene_limits {};

bool bounded_string(const char* value, std::size_t maximum, std::string& result)
{
    if (value == nullptr) {
        return false;
    }
    const std::size_t length = strnlen(value, maximum + 1);
    if (length == 0 || length > maximum) {
        return false;
    }
    result.assign(value, length);
    return true;
}

std::string configured_socket_path(const char* configured)
{
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    const char* environment_socket = std::getenv("MANGO_OVERLAY_SOCKET");
    if (environment_socket != nullptr && environment_socket[0] != '\0') {
        return environment_socket;
    }
    const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
    const std::string base = runtime_directory != nullptr && runtime_directory[0] != '\0'
        ? runtime_directory
        : "/run/user/" + std::to_string(getuid());
    return base + "/mango-overlay-decky.sock";
}

mango_overlay::scene::Visibility scene_visibility(mango_overlay_visibility visibility)
{
    switch (visibility) {
    case MANGO_OVERLAY_VISIBILITY_GAME_ONLY:
        return mango_overlay::scene::Visibility::game_only;
    case MANGO_OVERLAY_VISIBILITY_STEAM_ONLY:
        return mango_overlay::scene::Visibility::steam_only;
    case MANGO_OVERLAY_VISIBILITY_ALWAYS:
        return mango_overlay::scene::Visibility::always;
    }
    return mango_overlay::scene::Visibility::game_only;
}

mango_overlay::scene::Color scene_color(mango_overlay_color color)
{
    return { color.red, color.green, color.blue, color.alpha };
}

std::optional<mango_overlay::scene::Anchor> scene_anchor(mango_overlay_anchor anchor)
{
    switch (anchor) {
    case MANGO_OVERLAY_ANCHOR_TOP_LEFT:
        return mango_overlay::scene::Anchor::top_left;
    case MANGO_OVERLAY_ANCHOR_TOP_CENTER:
        return mango_overlay::scene::Anchor::top_center;
    case MANGO_OVERLAY_ANCHOR_TOP_RIGHT:
        return mango_overlay::scene::Anchor::top_right;
    case MANGO_OVERLAY_ANCHOR_CENTER_LEFT:
        return mango_overlay::scene::Anchor::center_left;
    case MANGO_OVERLAY_ANCHOR_CENTER:
        return mango_overlay::scene::Anchor::center;
    case MANGO_OVERLAY_ANCHOR_CENTER_RIGHT:
        return mango_overlay::scene::Anchor::center_right;
    case MANGO_OVERLAY_ANCHOR_BOTTOM_LEFT:
        return mango_overlay::scene::Anchor::bottom_left;
    case MANGO_OVERLAY_ANCHOR_BOTTOM_CENTER:
        return mango_overlay::scene::Anchor::bottom_center;
    case MANGO_OVERLAY_ANCHOR_BOTTOM_RIGHT:
        return mango_overlay::scene::Anchor::bottom_right;
    }
    return std::nullopt;
}

MangoOverlay::Wire::Visibility wire_visibility(mango_overlay_visibility visibility)
{
    switch (visibility) {
    case MANGO_OVERLAY_VISIBILITY_GAME_ONLY:
        return MangoOverlay::Wire::Visibility::GameOnly;
    case MANGO_OVERLAY_VISIBILITY_STEAM_ONLY:
        return MangoOverlay::Wire::Visibility::SteamOnly;
    case MANGO_OVERLAY_VISIBILITY_ALWAYS:
        return MangoOverlay::Wire::Visibility::Always;
    }
    return MangoOverlay::Wire::Visibility::GameOnly;
}

} // namespace

struct mango_overlay_client {
    int socket_fd = -1;
    ProtocolVersion negotiated_version = protocol_version;
    std::uint64_t next_request_id = 1;
    std::uint64_t next_transaction_id = 1;
    std::uint64_t active_transaction_id = 0;
    bool registered = false;
    std::vector<mango_overlay::scene::SceneMutation> mutations;
    std::vector<std::uint8_t> receive_buffer;
    std::string last_error;

    ~mango_overlay_client()
    {
        if (socket_fd >= 0) {
            close(socket_fd);
        }
    }
};

namespace {

mango_overlay_result fail(
    mango_overlay_client* client,
    mango_overlay_result result,
    const char* message) noexcept
{
    if (client != nullptr) {
        try {
            client->last_error = message;
        } catch (...) {
        }
    }
    return result;
}

template <typename Callback>
mango_overlay_result guarded(
    mango_overlay_client* client,
    Callback&& callback) noexcept
{
    try {
        return callback();
    } catch (const std::bad_alloc&) {
        return fail(client, MANGO_OVERLAY_OUT_OF_MEMORY, "The client ran out of memory");
    } catch (...) {
        return fail(client, MANGO_OVERLAY_PROTOCOL_ERROR, "The client encountered an internal error");
    }
}

void clear_error(mango_overlay_client* client)
{
    if (client != nullptr) {
        client->last_error.clear();
    }
}

void close_connection(mango_overlay_client* client)
{
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }
    client->registered = false;
    client->active_transaction_id = 0;
    client->mutations.clear();
}

mango_overlay_result fail_io(mango_overlay_client* client, const char* message)
{
    close_connection(client);
    return fail(client, MANGO_OVERLAY_IO_ERROR, message);
}

mango_overlay_result fail_protocol(mango_overlay_client* client, const char* message)
{
    close_connection(client);
    return fail(client, MANGO_OVERLAY_PROTOCOL_ERROR, message);
}

mango_overlay_result retain_element(
    mango_overlay_client* client,
    mango_overlay::scene::Element element,
    const char* invalid_message,
    const char* allocation_message)
{
    if (!mango_overlay::scene::valid_scene_element(element, scene_limits)) {
        return fail(client, MANGO_OVERLAY_INVALID_ARGUMENT, invalid_message);
    }
    if (client->mutations.size() >= scene_limits.maximum_mutations_per_transaction) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "The transaction mutation limit was reached");
    }
    try {
        client->mutations.push_back(
            mango_overlay::scene::UpsertElement { std::move(element) });
    } catch (const std::bad_alloc&) {
        return fail(client, MANGO_OVERLAY_OUT_OF_MEMORY, allocation_message);
    }
    return MANGO_OVERLAY_OK;
}

bool apply_layout(
    const mango_overlay_element_layout* layout,
    mango_overlay::scene::Element& element)
{
    if (layout == nullptr) {
        return true;
    }
    const auto anchor = scene_anchor(layout->anchor);
    if (layout->struct_size < sizeof(*layout) || !anchor.has_value()
        || layout->hidden > 1 || layout->clip_enabled > 1 || layout->reserved != 0) {
        return false;
    }
    element.parent_id = layout->parent_id;
    element.transform.translation = {
        layout->translation.x, layout->translation.y };
    element.transform.scale = { layout->scale.x, layout->scale.y };
    element.transform.rotation_degrees = layout->rotation_degrees;
    element.transform.opacity = layout->opacity;
    element.transform.anchor = *anchor;
    element.transform.hidden = layout->hidden != 0;
    if (layout->clip_enabled != 0) {
        element.transform.clip = mango_overlay::scene::ClipRect {
            { layout->clip.x, layout->clip.y },
            { layout->clip.width, layout->clip.height },
        };
    }
    return true;
}

mango_overlay_result retain_element_with_layout(
    mango_overlay_client* client,
    mango_overlay::scene::Element element,
    const mango_overlay_element_layout* layout,
    const char* invalid_message,
    const char* allocation_message)
{
    if (!apply_layout(layout, element)) {
        return fail(client, MANGO_OVERLAY_INVALID_ARGUMENT, invalid_message);
    }
    return retain_element(
        client,
        std::move(element),
        invalid_message,
        allocation_message);
}

int connect_socket(const std::string& path, std::uint32_t timeout_ms)
{
    sockaddr_un address {};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return -1;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return -1;
    }
    const timeval timeout {
        static_cast<time_t>(timeout_ms / 1000),
        static_cast<suseconds_t>((timeout_ms % 1000) * 1000),
    };
    if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0
        || setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(descriptor);
        return -1;
    }

    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (connect(descriptor, reinterpret_cast<sockaddr*>(&address), address_size) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

bool send_bytes(
    int descriptor,
    const std::vector<std::uint8_t>& packet,
    int attachment_fd = -1)
{
    return mango_overlay::protocol::send_seqpacket(
               descriptor,
               ByteView { packet.data(), packet.size() },
               attachment_fd)
        == mango_overlay::protocol::SeqpacketResult::accepted;
}

mango_overlay_result decode_server_error(
    mango_overlay_client* client,
    ByteView payload)
{
    flatbuffers::Verifier verifier(payload.data, payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ErrorResponse>(nullptr)) {
        return fail_protocol(client, "mango-overlayd returned a malformed error");
    }
    const auto* response = flatbuffers::GetRoot<MangoOverlay::Wire::ErrorResponse>(
        payload.data);
    if (response->message() == nullptr || response->message()->size() == 0) {
        return fail(client, MANGO_OVERLAY_SERVER_REJECTED, "mango-overlayd rejected the request");
    }
    client->last_error = response->message()->str();
    return MANGO_OVERLAY_SERVER_REJECTED;
}

mango_overlay_result exchange(
    mango_overlay_client* client,
    const std::vector<std::uint8_t>& request,
    MessageType expected_type,
    std::uint64_t request_id,
    DecodedPacketView& response,
    int attachment_fd = -1)
{
    if (!send_bytes(client->socket_fd, request, attachment_fd)) {
        return fail_io(client, "Could not send a request to mango-overlayd");
    }

    client->receive_buffer.resize(
        mango_overlay::protocol::packet_header_size
        + mango_overlay::protocol::maximum_payload_size);
    std::size_t received = 0;
    mango_overlay::protocol::UniqueFileDescriptor response_attachment;
    if (mango_overlay::protocol::receive_seqpacket(
            client->socket_fd,
            client->receive_buffer.data(),
            client->receive_buffer.size(),
            received,
            response_attachment)
        != mango_overlay::protocol::SeqpacketResult::accepted) {
        return fail_io(client, "Could not receive a response from mango-overlayd");
    }
    client->receive_buffer.resize(received);

    const auto decoded = mango_overlay::protocol::decode_packet(ByteView {
        client->receive_buffer.data(), client->receive_buffer.size() });
    const auto* packet = std::get_if<DecodedPacketView>(&decoded);
    if (packet == nullptr || packet->header.version.major != client->negotiated_version.major
        || packet->header.version.minor != client->negotiated_version.minor
        || packet->header.request_id != request_id || packet->header.flags != 0) {
        return fail_protocol(client, "mango-overlayd returned an invalid response packet");
    }
    if (packet->header.message_type == MessageType::error) {
        return decode_server_error(client, packet->payload);
    }
    if (packet->header.message_type != expected_type) {
        return fail_protocol(client, "mango-overlayd returned an unexpected response");
    }
    response = *packet;
    return MANGO_OVERLAY_OK;
}

std::vector<std::uint8_t> packet_for(
    mango_overlay_client* client,
    MessageType type,
    std::uint64_t request_id,
    const std::uint8_t* payload,
    std::size_t payload_size,
    std::uint16_t flags = 0)
{
    return mango_overlay::protocol::encode_packet(
        PacketHeader { client->negotiated_version, type, flags, request_id },
        ByteView { payload, payload_size });
}

bool valid_structure(std::uint32_t actual, std::size_t expected)
{
    return actual >= expected;
}

} // namespace

extern "C" {

uint32_t mango_overlay_client_abi_version(void)
{
    return MANGO_OVERLAY_CLIENT_ABI_VERSION;
}

static mango_overlay_result open_impl(
    const mango_overlay_client_config* config,
    mango_overlay_client** out_client)
{
    if (out_client == nullptr) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    *out_client = nullptr;
    if (config == nullptr || !valid_structure(config->struct_size, sizeof(*config))
        || config->client_version == nullptr || config->timeout_ms > maximum_timeout_ms) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }

    std::string client_version;
    if (!bounded_string(config->client_version, maximum_string_bytes, client_version)) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    const std::uint32_t timeout = config->timeout_ms == 0
        ? default_timeout_ms
        : config->timeout_ms;
    const std::string socket_path = configured_socket_path(config->socket_path);

    std::unique_ptr<mango_overlay_client> client;
    try {
        client = std::make_unique<mango_overlay_client>();
        client->receive_buffer.reserve(
            mango_overlay::protocol::packet_header_size
            + mango_overlay::protocol::maximum_payload_size);
    } catch (const std::bad_alloc&) {
        return MANGO_OVERLAY_OUT_OF_MEMORY;
    }

    client->socket_fd = connect_socket(socket_path, timeout);
    if (client->socket_fd < 0) {
        return MANGO_OVERLAY_CONNECTION_FAILED;
    }

    const std::uint64_t request_id = client->next_request_id++;
    const auto hello_payload = mango_overlay::protocol::encode_hello({
        mango_overlay::protocol::ConnectionRole::provider,
        protocol_version,
        protocol_version,
        mango_overlay::protocol::capability::retained_scene_transactions
            | mango_overlay::protocol::capability::image_resources,
        0,
        client_version,
    });
    const auto hello = packet_for(
        client.get(),
        MessageType::hello,
        request_id,
        hello_payload.data(),
        hello_payload.size());
    DecodedPacketView response {};
    const auto exchange_result = exchange(
        client.get(), hello, MessageType::hello_accepted, request_id, response);
    if (exchange_result != MANGO_OVERLAY_OK) {
        close_connection(client.get());
        return exchange_result;
    }
    const auto accepted = mango_overlay::protocol::decode_hello_accepted(response.payload);
    const auto* handshake = std::get_if<mango_overlay::protocol::HelloAccepted>(&accepted);
    if (handshake == nullptr
        || handshake->selected_version.major != protocol_version.major
        || handshake->selected_version.minor != protocol_version.minor
        || (handshake->enabled_capabilities
               & mango_overlay::protocol::capability::retained_scene_transactions)
            == 0
        || (handshake->enabled_capabilities
               & mango_overlay::protocol::capability::image_resources)
            == 0) {
        close_connection(client.get());
        return MANGO_OVERLAY_PROTOCOL_ERROR;
    }
    client->negotiated_version = handshake->selected_version;
    *out_client = client.release();
    return MANGO_OVERLAY_OK;
}

static void close_impl(mango_overlay_client* client)
{
    if (client != nullptr) {
        close_connection(client);
        delete client;
    }
}

static mango_overlay_result register_provider_impl(
    mango_overlay_client* client,
    const mango_overlay_provider_info* provider)
{
    if (client == nullptr || provider == nullptr
        || !valid_structure(provider->struct_size, sizeof(*provider))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->socket_fd < 0 || client->registered || client->active_transaction_id != 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "Provider registration is not allowed now");
    }

    std::string application_id;
    std::string instance_id;
    std::string display_name;
    const bool valid_visibility = provider->visibility == MANGO_OVERLAY_VISIBILITY_GAME_ONLY
        || provider->visibility == MANGO_OVERLAY_VISIBILITY_STEAM_ONLY
        || provider->visibility == MANGO_OVERLAY_VISIBILITY_ALWAYS;
    if (!bounded_string(provider->application_id, maximum_string_bytes, application_id)
        || !bounded_string(provider->instance_id, maximum_string_bytes, instance_id)
        || !bounded_string(provider->display_name, maximum_string_bytes, display_name)
        || !valid_visibility) {
        return fail(client, MANGO_OVERLAY_INVALID_ARGUMENT, "Provider information is invalid");
    }
    const mango_overlay::scene::ProviderIdentity identity {
        application_id,
        instance_id,
        display_name,
        provider->canvas_width,
        provider->canvas_height,
        scene_visibility(provider->visibility),
    };
    if (!mango_overlay::scene::valid_provider_identity(identity)) {
        return fail(client, MANGO_OVERLAY_INVALID_ARGUMENT, "Provider information is invalid");
    }

    flatbuffers::FlatBufferBuilder builder;
    const auto registration = MangoOverlay::Wire::CreateRegisterProvider(
        builder,
        builder.CreateString(application_id),
        builder.CreateString(instance_id),
        builder.CreateString(display_name),
        provider->canvas_width,
        provider->canvas_height,
        wire_visibility(provider->visibility));
    builder.Finish(registration);
    const std::uint64_t request_id = client->next_request_id++;
    const auto request = packet_for(
        client,
        MessageType::register_provider,
        request_id,
        builder.GetBufferPointer(),
        builder.GetSize());
    DecodedPacketView response {};
    const auto result = exchange(
        client, request, MessageType::provider_registered, request_id, response);
    if (result != MANGO_OVERLAY_OK) {
        return result;
    }
    flatbuffers::Verifier verifier(response.payload.data, response.payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ProviderRegistered>(nullptr)) {
        return fail_protocol(client, "mango-overlayd returned malformed registration data");
    }
    client->registered = true;
    return MANGO_OVERLAY_OK;
}

static mango_overlay_result upload_resource_request(
    mango_overlay_client* client,
    std::uint64_t resource_id,
    const std::uint8_t* inline_data,
    std::uint32_t encoded_size,
    int attachment_fd)
{
    if (client == nullptr || resource_id == 0 || encoded_size == 0
        || encoded_size > scene_limits.maximum_encoded_resource_bytes
        || (inline_data == nullptr) == (attachment_fd < 0)) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->socket_fd < 0 || !client->registered
        || client->active_transaction_id != 0) {
        return fail(
            client,
            MANGO_OVERLAY_INVALID_STATE,
            "Resources can only be uploaded outside a scene transaction");
    }

    flatbuffers::FlatBufferBuilder builder;
    flatbuffers::Offset<flatbuffers::Vector<std::uint8_t>> wire_data;
    if (inline_data != nullptr) {
        wire_data = builder.CreateVector(inline_data, encoded_size);
    }
    const auto upload = MangoOverlay::Wire::CreateUploadResource(
        builder, resource_id, encoded_size, wire_data);
    builder.Finish(upload);
    const std::uint64_t request_id = client->next_request_id++;
    const auto request = packet_for(
        client,
        MessageType::upload_resource,
        request_id,
        builder.GetBufferPointer(),
        builder.GetSize(),
        attachment_fd >= 0
            ? mango_overlay::protocol::packet_flag_file_descriptor
            : 0);
    DecodedPacketView response {};
    const auto result = exchange(
        client,
        request,
        MessageType::resource_stored,
        request_id,
        response,
        attachment_fd);
    if (result != MANGO_OVERLAY_OK) {
        return result;
    }
    flatbuffers::Verifier verifier(response.payload.data, response.payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ResourceStored>(nullptr)) {
        return fail_protocol(client, "mango-overlayd returned malformed resource data");
    }
    const auto* stored
        = flatbuffers::GetRoot<MangoOverlay::Wire::ResourceStored>(response.payload.data);
    if (stored->resource_id() != resource_id || stored->width() == 0
        || stored->height() == 0 || stored->frame_count() == 0) {
        return fail_protocol(client, "mango-overlayd acknowledged a different resource");
    }
    return MANGO_OVERLAY_OK;
}

static mango_overlay_result upload_resource_impl(
    mango_overlay_client* client,
    std::uint64_t resource_id,
    const void* encoded_data,
    std::uint32_t encoded_size)
{
    if (encoded_data == nullptr || encoded_size == 0) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(encoded_data);
    if (encoded_size <= inline_resource_threshold) {
        return upload_resource_request(
            client, resource_id, bytes, encoded_size, -1);
    }
    auto descriptor = mango_overlay::protocol::make_sealed_memfd(
        ByteView { bytes, encoded_size });
    if (!descriptor) {
        return fail(client, MANGO_OVERLAY_IO_ERROR, "Could not prepare the image resource");
    }
    return upload_resource_request(
        client, resource_id, nullptr, encoded_size, descriptor.get());
}

static mango_overlay_result upload_resource_fd_impl(
    mango_overlay_client* client,
    std::uint64_t resource_id,
    int descriptor,
    std::uint32_t encoded_size)
{
    if (descriptor < 0) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    return upload_resource_request(
        client, resource_id, nullptr, encoded_size, descriptor);
}

static mango_overlay_result release_resource_impl(
    mango_overlay_client* client,
    std::uint64_t resource_id)
{
    if (client == nullptr || resource_id == 0) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->socket_fd < 0 || !client->registered
        || client->active_transaction_id != 0) {
        return fail(
            client,
            MANGO_OVERLAY_INVALID_STATE,
            "Resources can only be released outside a scene transaction");
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto release = MangoOverlay::Wire::CreateReleaseResource(
        builder, resource_id);
    builder.Finish(release);
    const std::uint64_t request_id = client->next_request_id++;
    const auto request = packet_for(
        client,
        MessageType::release_resource,
        request_id,
        builder.GetBufferPointer(),
        builder.GetSize());
    DecodedPacketView response {};
    const auto result = exchange(
        client,
        request,
        MessageType::resource_released,
        request_id,
        response);
    if (result != MANGO_OVERLAY_OK) {
        return result;
    }
    flatbuffers::Verifier verifier(response.payload.data, response.payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::ResourceReleased>(nullptr)
        || flatbuffers::GetRoot<MangoOverlay::Wire::ResourceReleased>(
               response.payload.data)
                ->resource_id()
            != resource_id) {
        return fail_protocol(client, "mango-overlayd returned malformed resource data");
    }
    return MANGO_OVERLAY_OK;
}

static mango_overlay_result begin_transaction_impl(mango_overlay_client* client)
{
    if (client == nullptr) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->socket_fd < 0 || !client->registered || client->active_transaction_id != 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "A transaction cannot begin now");
    }
    client->mutations.clear();
    client->active_transaction_id = client->next_transaction_id++;
    return MANGO_OVERLAY_OK;
}

static mango_overlay_result upsert_group_impl(
    mango_overlay_client* client,
    const mango_overlay_group_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    return retain_element_with_layout(
        client,
        mango_overlay::scene::Element {
            element->element_id,
            element->z_index,
            mango_overlay::scene::GroupElement {},
        },
        element->layout,
        "Group element is invalid",
        "Could not retain the group element");
}

static mango_overlay_result upsert_text_impl(
    mango_overlay_client* client,
    const mango_overlay_text_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }

    std::string text;
    if (!bounded_string(element->text, maximum_text_bytes, text)) {
        return fail(client, MANGO_OVERLAY_INVALID_ARGUMENT, "Text element is invalid");
    }
    mango_overlay::scene::Element scene_element {
        element->element_id,
        element->z_index,
        mango_overlay::scene::TextElement {
            { element->x, element->y },
            std::move(text),
            element->font_size,
            scene_color(element->color),
        },
    };
    return retain_element_with_layout(
        client,
        std::move(scene_element),
        element->layout,
        "Text element is invalid",
        "Could not retain the text element");
}

static mango_overlay_result upsert_rectangle_impl(
    mango_overlay_client* client,
    const mango_overlay_rectangle_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    mango_overlay::scene::Element scene_element {
        element->element_id,
        element->z_index,
        mango_overlay::scene::RectangleElement {
            { element->x, element->y },
            { element->width, element->height },
            element->corner_radius,
            scene_color(element->color),
        },
    };
    return retain_element_with_layout(
        client,
        std::move(scene_element),
        element->layout,
        "Rectangle element is invalid",
        "Could not retain the rectangle element");
}

static mango_overlay_result upsert_line_impl(
    mango_overlay_client* client,
    const mango_overlay_line_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    return retain_element_with_layout(
        client,
        mango_overlay::scene::Element {
            element->element_id,
            element->z_index,
            mango_overlay::scene::LineElement {
                { element->start.x, element->start.y },
                { element->end.x, element->end.y },
                element->thickness,
                scene_color(element->color),
            },
        },
        element->layout,
        "Line element is invalid",
        "Could not retain the line element");
}

static mango_overlay_result upsert_polyline_impl(
    mango_overlay_client* client,
    const mango_overlay_polyline_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    if (element->points == nullptr || element->point_count < 2
        || element->point_count > scene_limits.maximum_polyline_points) {
        return fail(client, MANGO_OVERLAY_INVALID_ARGUMENT, "Polyline element is invalid");
    }

    std::vector<mango_overlay::scene::Vec2> points;
    points.reserve(element->point_count);
    for (std::uint32_t index = 0; index < element->point_count; ++index) {
        points.push_back({ element->points[index].x, element->points[index].y });
    }
    return retain_element_with_layout(
        client,
        mango_overlay::scene::Element {
            element->element_id,
            element->z_index,
            mango_overlay::scene::PolylineElement {
                std::move(points), element->thickness, scene_color(element->color) },
        },
        element->layout,
        "Polyline element is invalid",
        "Could not retain the polyline element");
}

static mango_overlay_result upsert_circle_impl(
    mango_overlay_client* client,
    const mango_overlay_circle_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    return retain_element_with_layout(
        client,
        mango_overlay::scene::Element {
            element->element_id,
            element->z_index,
            mango_overlay::scene::CircleElement {
                { element->center.x, element->center.y },
                element->radius,
                scene_color(element->color),
            },
        },
        element->layout,
        "Circle element is invalid",
        "Could not retain the circle element");
}

static mango_overlay_result upsert_image_impl(
    mango_overlay_client* client,
    const mango_overlay_image_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    return retain_element_with_layout(
        client,
        mango_overlay::scene::Element {
            element->element_id,
            element->z_index,
            mango_overlay::scene::ImageElement {
                { element->x, element->y },
                { element->width, element->height },
                element->resource_id,
                scene_color(element->tint),
            },
        },
        element->layout,
        "Image element is invalid",
        "Could not retain the image element");
}

static mango_overlay_result upsert_gif_impl(
    mango_overlay_client* client,
    const mango_overlay_gif_element* element)
{
    if (client == nullptr || element == nullptr
        || !valid_structure(element->struct_size, sizeof(*element))
        || element->paused > 1 || element->reserved[0] != 0
        || element->reserved[1] != 0 || element->reserved[2] != 0) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    return retain_element_with_layout(
        client,
        mango_overlay::scene::Element {
            element->element_id,
            element->z_index,
            mango_overlay::scene::GifElement {
                { element->x, element->y },
                { element->width, element->height },
                element->resource_id,
                scene_color(element->tint),
                element->playback_rate,
                element->paused != 0,
                element->frame_index,
            },
        },
        element->layout,
        "GIF element is invalid",
        "Could not retain the GIF element");
}

static mango_overlay_result remove_element_impl(
    mango_overlay_client* client,
    uint64_t element_id)
{
    if (client == nullptr || element_id == 0) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    if (client->mutations.size() >= scene_limits.maximum_mutations_per_transaction) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "The transaction mutation limit was reached");
    }
    try {
        client->mutations.push_back(mango_overlay::scene::RemoveElement { element_id });
    } catch (const std::bad_alloc&) {
        return fail(client, MANGO_OVERLAY_OUT_OF_MEMORY, "Could not retain the element removal");
    }
    return MANGO_OVERLAY_OK;
}

static mango_overlay_result commit_transaction_impl(mango_overlay_client* client)
{
    if (client == nullptr) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->socket_fd < 0 || !client->registered
        || client->active_transaction_id == 0 || client->mutations.empty()) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No non-empty transaction is active");
    }

    const std::uint64_t transaction_id = client->active_transaction_id;
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<MangoOverlay::Wire::Mutation>> mutations;
    try {
        mutations.reserve(client->mutations.size());
        for (const auto& mutation : client->mutations) {
            mutations.push_back(mango_overlay::wire::encode_mutation(builder, mutation));
        }
    } catch (const std::bad_alloc&) {
        return fail(client, MANGO_OVERLAY_OUT_OF_MEMORY, "Could not encode the transaction");
    }
    const auto wire_mutations = builder.CreateVector(mutations);
    const auto transaction = MangoOverlay::Wire::CreateSceneTransaction(
        builder, transaction_id, wire_mutations);
    builder.Finish(transaction);

    const std::uint64_t request_id = client->next_request_id++;
    const auto request = packet_for(
        client,
        MessageType::scene_transaction,
        request_id,
        builder.GetBufferPointer(),
        builder.GetSize());
    DecodedPacketView response {};
    const auto result = exchange(
        client, request, MessageType::transaction_committed, request_id, response);
    client->active_transaction_id = 0;
    client->mutations.clear();
    if (result != MANGO_OVERLAY_OK) {
        return result;
    }

    flatbuffers::Verifier verifier(response.payload.data, response.payload.size);
    if (!verifier.VerifyBuffer<MangoOverlay::Wire::TransactionCommitted>(nullptr)) {
        return fail_protocol(client, "mango-overlayd returned malformed commit data");
    }
    const auto* committed = flatbuffers::GetRoot<MangoOverlay::Wire::TransactionCommitted>(
        response.payload.data);
    if (committed->transaction_id() != transaction_id) {
        return fail_protocol(client, "mango-overlayd acknowledged a different transaction");
    }
    return MANGO_OVERLAY_OK;
}

static mango_overlay_result abort_transaction_impl(mango_overlay_client* client)
{
    if (client == nullptr) {
        return MANGO_OVERLAY_INVALID_ARGUMENT;
    }
    clear_error(client);
    if (client->active_transaction_id == 0) {
        return fail(client, MANGO_OVERLAY_INVALID_STATE, "No transaction is active");
    }
    client->active_transaction_id = 0;
    client->mutations.clear();
    return MANGO_OVERLAY_OK;
}

mango_overlay_result mango_overlay_client_open(
    const mango_overlay_client_config* config,
    mango_overlay_client** out_client)
{
    return guarded(nullptr, [&] { return open_impl(config, out_client); });
}

void mango_overlay_client_close(mango_overlay_client* client)
{
    try {
        close_impl(client);
    } catch (...) {
    }
}

mango_overlay_result mango_overlay_client_register_provider(
    mango_overlay_client* client,
    const mango_overlay_provider_info* provider)
{
    return guarded(client, [&] { return register_provider_impl(client, provider); });
}

mango_overlay_result mango_overlay_client_upload_resource(
    mango_overlay_client* client,
    uint64_t resource_id,
    const void* encoded_data,
    uint32_t encoded_size)
{
    return guarded(client, [&] {
        return upload_resource_impl(
            client, resource_id, encoded_data, encoded_size);
    });
}

mango_overlay_result mango_overlay_client_upload_resource_fd(
    mango_overlay_client* client,
    uint64_t resource_id,
    int descriptor,
    uint32_t encoded_size)
{
    return guarded(client, [&] {
        return upload_resource_fd_impl(
            client, resource_id, descriptor, encoded_size);
    });
}

mango_overlay_result mango_overlay_client_release_resource(
    mango_overlay_client* client,
    uint64_t resource_id)
{
    return guarded(client, [&] {
        return release_resource_impl(client, resource_id);
    });
}

mango_overlay_result mango_overlay_client_begin_transaction(mango_overlay_client* client)
{
    return guarded(client, [&] { return begin_transaction_impl(client); });
}

mango_overlay_result mango_overlay_client_upsert_group(
    mango_overlay_client* client,
    const mango_overlay_group_element* element)
{
    return guarded(client, [&] { return upsert_group_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_text(
    mango_overlay_client* client,
    const mango_overlay_text_element* element)
{
    return guarded(client, [&] { return upsert_text_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_rectangle(
    mango_overlay_client* client,
    const mango_overlay_rectangle_element* element)
{
    return guarded(client, [&] { return upsert_rectangle_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_line(
    mango_overlay_client* client,
    const mango_overlay_line_element* element)
{
    return guarded(client, [&] { return upsert_line_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_polyline(
    mango_overlay_client* client,
    const mango_overlay_polyline_element* element)
{
    return guarded(client, [&] { return upsert_polyline_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_circle(
    mango_overlay_client* client,
    const mango_overlay_circle_element* element)
{
    return guarded(client, [&] { return upsert_circle_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_image(
    mango_overlay_client* client,
    const mango_overlay_image_element* element)
{
    return guarded(client, [&] { return upsert_image_impl(client, element); });
}

mango_overlay_result mango_overlay_client_upsert_gif(
    mango_overlay_client* client,
    const mango_overlay_gif_element* element)
{
    return guarded(client, [&] { return upsert_gif_impl(client, element); });
}

mango_overlay_result mango_overlay_client_remove_element(
    mango_overlay_client* client,
    uint64_t element_id)
{
    return guarded(client, [&] { return remove_element_impl(client, element_id); });
}

mango_overlay_result mango_overlay_client_commit_transaction(mango_overlay_client* client)
{
    return guarded(client, [&] { return commit_transaction_impl(client); });
}

mango_overlay_result mango_overlay_client_abort_transaction(mango_overlay_client* client)
{
    return guarded(client, [&] { return abort_transaction_impl(client); });
}

const char* mango_overlay_client_last_error(const mango_overlay_client* client)
{
    return client == nullptr ? "Client handle is null" : client->last_error.c_str();
}

} // extern "C"
