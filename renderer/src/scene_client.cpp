#include "mango_overlay/renderer/scene_client.hpp"
#include "mango_overlay/protocol/handshake.hpp"
#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/renderer.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"
#include "mango_overlay/renderer/scene_mirror.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace mango_overlay::renderer {

namespace {

constexpr protocol::ProtocolVersion protocol_version { 1, 0 };
constexpr std::uint64_t hello_request_id = 1;
constexpr std::uint64_t subscription_request_id = 2;

int connect_socket(const std::string& path)
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
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (connect(descriptor, reinterpret_cast<sockaddr*>(&address), address_size) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

bool send_packet(int descriptor, const std::vector<std::uint8_t>& packet)
{
    return protocol::send_seqpacket(
               descriptor,
               protocol::ByteView { packet.data(), packet.size() })
        == protocol::SeqpacketResult::accepted;
}

bool receive_packet(
    int descriptor,
    std::vector<std::uint8_t>& packet,
    protocol::UniqueFileDescriptor& attachment)
{
    packet.resize(protocol::packet_header_size + protocol::maximum_payload_size);
    std::size_t received = 0;
    if (protocol::receive_seqpacket(
            descriptor,
            packet.data(),
            packet.size(),
            received,
            attachment)
        != protocol::SeqpacketResult::accepted) {
        packet.clear();
        return false;
    }
    packet.resize(received);
    return true;
}

std::vector<std::uint8_t> make_hello_packet()
{
    const auto payload = protocol::encode_hello(protocol::Hello {
        protocol::ConnectionRole::renderer,
        protocol_version,
        protocol_version,
        protocol::capability::renderer_scene_stream
            | protocol::capability::image_resources,
        0,
        "mango-overlay-renderer/" MANGO_OVERLAY_VERSION,
    });
    return protocol::encode_packet(
        protocol::PacketHeader {
            protocol_version,
            protocol::MessageType::hello,
            0,
            hello_request_id,
        },
        protocol::ByteView { payload.data(), payload.size() });
}

std::vector<std::uint8_t> make_subscription_packet(std::uint64_t known_revision)
{
    const auto payload = protocol::encode_renderer_subscription(
        protocol::RendererSubscription { known_revision });
    return protocol::encode_packet(
        protocol::PacketHeader {
            protocol_version,
            protocol::MessageType::renderer_subscribe,
            0,
            subscription_request_id,
        },
        protocol::ByteView { payload.data(), payload.size() });
}

bool accept_handshake(const std::vector<std::uint8_t>& packet_bytes)
{
    const auto decoded = protocol::decode_packet(
        protocol::ByteView { packet_bytes.data(), packet_bytes.size() });
    const auto* packet = std::get_if<protocol::DecodedPacketView>(&decoded);
    if (packet == nullptr
        || packet->header.message_type != protocol::MessageType::hello_accepted
        || packet->header.request_id != hello_request_id || packet->header.flags != 0) {
        return false;
    }

    const auto payload = protocol::decode_hello_accepted(packet->payload);
    const auto* accepted = std::get_if<protocol::HelloAccepted>(&payload);
    return accepted != nullptr
        && accepted->selected_version.major == protocol_version.major
        && accepted->selected_version.minor == protocol_version.minor
        && (accepted->enabled_capabilities
               & protocol::capability::renderer_scene_stream)
            != 0
        && (accepted->enabled_capabilities
               & protocol::capability::image_resources)
            != 0;
}

} // namespace

struct SceneClient::Impl {
    Impl(std::string configured_path, std::function<void()> configured_callback)
        : path(std::move(configured_path))
        , on_scene_changed(std::move(configured_callback))
        , mirror(protocol_version)
    {
    }

    ~Impl()
    {
        stop();
    }

    void notify_scene_changed() const
    {
        if (!on_scene_changed) {
            return;
        }
        try {
            on_scene_changed();
        } catch (...) {
        }
    }

    bool run_connection(int descriptor)
    {
        std::vector<std::uint8_t> packet;
        protocol::UniqueFileDescriptor attachment;
        if (!send_packet(descriptor, make_hello_packet())
            || !receive_packet(descriptor, packet, attachment)
            || !accept_handshake(packet)
            || !send_packet(
                descriptor,
                make_subscription_packet(mirror.snapshot()->revision))) {
            return false;
        }

        is_connected.store(true, std::memory_order_release);
        while (!stopping.load(std::memory_order_acquire)) {
            if (!receive_packet(descriptor, packet, attachment)) {
                return false;
            }
            const auto result = mirror.apply_packet(
                protocol::ByteView { packet.data(), packet.size() }, attachment.get());
            if (result == ApplyResult::published) {
                notify_scene_changed();
            } else if (result == ApplyResult::revision_gap) {
                if (!send_packet(
                        descriptor,
                        make_subscription_packet(mirror.snapshot()->revision))) {
                    return false;
                }
            } else if (result != ApplyResult::accepted) {
                return false;
            }
        }
        return true;
    }

    void worker_main()
    {
        while (!stopping.load(std::memory_order_acquire)) {
            const int descriptor = connect_socket(path);
            if (descriptor >= 0) {
                {
                    std::lock_guard<std::mutex> lock(socket_mutex);
                    current_socket = descriptor;
                }
                if (!stopping.load(std::memory_order_acquire)) {
                    run_connection(descriptor);
                }
                is_connected.store(false, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(socket_mutex);
                    if (current_socket == descriptor) {
                        current_socket = -1;
                    }
                }
                close(descriptor);
            }

            std::unique_lock<std::mutex> lock(retry_mutex);
            retry_changed.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return stopping.load(std::memory_order_acquire);
            });
        }
    }

    void start()
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (worker.joinable()) {
            return;
        }
        stopping.store(false, std::memory_order_release);
        worker = std::thread([this] { worker_main(); });
    }

    void stop()
    {
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex);
        if (!worker.joinable()) {
            return;
        }
        stopping.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            if (current_socket >= 0) {
                shutdown(current_socket, SHUT_RDWR);
            }
        }
        retry_changed.notify_all();
        std::thread joining = std::move(worker);
        lifecycle_lock.unlock();
        joining.join();
        is_connected.store(false, std::memory_order_release);
    }

    std::string path;
    std::function<void()> on_scene_changed;
    SceneMirror mirror;
    std::atomic<bool> stopping { false };
    std::atomic<bool> is_connected { false };
    std::mutex lifecycle_mutex;
    std::mutex socket_mutex;
    int current_socket = -1;
    std::mutex retry_mutex;
    std::condition_variable retry_changed;
    std::thread worker;
};

std::string default_socket_path()
{
    const char* configured_socket = std::getenv("MANGO_OVERLAY_SOCKET");
    if (configured_socket != nullptr && configured_socket[0] != '\0') {
        return configured_socket;
    }
    const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_directory != nullptr && runtime_directory[0] != '\0') {
        return std::string(runtime_directory) + "/mango-overlay-decky.sock";
    }
    return "/run/user/" + std::to_string(getuid()) + "/mango-overlay-decky.sock";
}

SceneClient::SceneClient(
    std::string socket_path,
    std::function<void()> on_scene_changed)
    : impl_(std::make_unique<Impl>(
          std::move(socket_path), std::move(on_scene_changed)))
{
}

SceneClient::~SceneClient() = default;

void SceneClient::start()
{
    impl_->start();
}

void SceneClient::stop()
{
    impl_->stop();
}

bool SceneClient::connected() const
{
    return impl_->is_connected.load(std::memory_order_acquire);
}

std::shared_ptr<const scene::SceneSnapshot> SceneClient::snapshot() const
{
    return impl_->mirror.snapshot();
}

} // namespace mango_overlay::renderer
