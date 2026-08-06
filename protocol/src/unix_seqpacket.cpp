#include "mango_overlay/protocol/unix_seqpacket.hpp"

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <limits>
#include <utility>

namespace mango_overlay::protocol {

namespace {

constexpr int required_seals
    = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

bool packet_attachment_matches(ByteView packet, bool has_attachment)
{
    const auto decoded = decode_packet(packet);
    const auto* view = std::get_if<DecodedPacketView>(&decoded);
    if (view == nullptr) {
        return false;
    }
    const bool flag_set
        = (view->header.flags & packet_flag_file_descriptor) != 0;
    return flag_set == has_attachment
        && (view->header.flags & ~packet_flag_file_descriptor) == 0;
}

int create_memfd()
{
    return static_cast<int>(syscall(
        SYS_memfd_create,
        "mango-overlay-resource",
        MFD_CLOEXEC | MFD_ALLOW_SEALING));
}

bool write_all(int descriptor, ByteView bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size) {
        const auto written = write(
            descriptor,
            bytes.data + offset,
            bytes.size - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

void close_received_descriptors(msghdr& message, UniqueFileDescriptor& attachment)
{
    for (cmsghdr* control = CMSG_FIRSTHDR(&message); control != nullptr;
         control = CMSG_NXTHDR(&message, control)) {
        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const auto payload_size = control->cmsg_len >= CMSG_LEN(0)
            ? control->cmsg_len - CMSG_LEN(0)
            : 0;
        const auto count = payload_size / sizeof(int);
        const auto* descriptors = reinterpret_cast<const int*>(CMSG_DATA(control));
        for (std::size_t index = 0; index < count; ++index) {
            if (!attachment) {
                attachment.reset(descriptors[index]);
            } else {
                close(descriptors[index]);
            }
        }
    }
}

} // namespace

UniqueFileDescriptor::UniqueFileDescriptor(int descriptor) noexcept
    : descriptor_(descriptor)
{
}

UniqueFileDescriptor::~UniqueFileDescriptor()
{
    reset();
}

UniqueFileDescriptor::UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept
    : descriptor_(other.release())
{
}

UniqueFileDescriptor& UniqueFileDescriptor::operator=(
    UniqueFileDescriptor&& other) noexcept
{
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFileDescriptor::release() noexcept
{
    return std::exchange(descriptor_, -1);
}

void UniqueFileDescriptor::reset(int descriptor) noexcept
{
    if (descriptor_ >= 0) {
        close(descriptor_);
    }
    descriptor_ = descriptor;
}

SeqpacketResult send_seqpacket(
    int socket_fd,
    ByteView packet,
    int attachment_fd)
{
    if (!packet_attachment_matches(packet, attachment_fd >= 0)) {
        return SeqpacketResult::rejected;
    }

    iovec vector {
        const_cast<std::uint8_t*>(packet.data),
        packet.size,
    };
    msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;

    std::array<std::uint8_t, CMSG_SPACE(sizeof(int))> control {};
    if (attachment_fd >= 0) {
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        *reinterpret_cast<int*>(CMSG_DATA(header)) = attachment_fd;
    }

    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == static_cast<ssize_t>(packet.size)
        ? SeqpacketResult::accepted
        : SeqpacketResult::io_error;
}

SeqpacketResult receive_seqpacket(
    int socket_fd,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t& received_size,
    UniqueFileDescriptor& attachment)
{
    received_size = 0;
    attachment.reset();
    if (buffer == nullptr || capacity == 0) {
        return SeqpacketResult::rejected;
    }

    iovec vector { buffer, capacity };
    std::array<std::uint8_t, CMSG_SPACE(sizeof(int) * 2)> control {};
    msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    ssize_t received;
    do {
        received = recvmsg(socket_fd, &message, MSG_TRUNC | MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received == 0) {
        return SeqpacketResult::peer_closed;
    }
    if (received < 0) {
        return SeqpacketResult::io_error;
    }

    std::size_t descriptor_count = 0;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS
            && header->cmsg_len >= CMSG_LEN(0)) {
            descriptor_count += (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        }
    }
    close_received_descriptors(message, attachment);

    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0
        || static_cast<std::size_t>(received) > capacity || descriptor_count > 1) {
        attachment.reset();
        return SeqpacketResult::rejected;
    }
    received_size = static_cast<std::size_t>(received);
    const ByteView packet { buffer, received_size };
    if (!packet_attachment_matches(packet, static_cast<bool>(attachment))) {
        attachment.reset();
        return SeqpacketResult::rejected;
    }
    return SeqpacketResult::accepted;
}

UniqueFileDescriptor make_sealed_memfd(ByteView bytes)
{
    if (bytes.data == nullptr || bytes.size == 0
        || bytes.size > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
        return {};
    }
    UniqueFileDescriptor descriptor(create_memfd());
    if (!descriptor || !write_all(descriptor.get(), bytes)
        || fcntl(descriptor.get(), F_ADD_SEALS, required_seals) != 0) {
        return {};
    }
    return descriptor;
}

bool read_resource_descriptor(
    int descriptor,
    std::size_t expected_size,
    std::size_t maximum_size,
    std::vector<std::uint8_t>& bytes)
{
    bytes.clear();
    if (descriptor < 0 || expected_size == 0 || expected_size > maximum_size
        || expected_size > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
        return false;
    }

    struct stat status {};
    const int open_flags = fcntl(descriptor, F_GETFL);
    const int seals = fcntl(descriptor, F_GET_SEALS);
    const bool read_only = open_flags >= 0 && (open_flags & O_ACCMODE) == O_RDONLY;
    const bool sealed = seals >= 0 && (seals & required_seals) == required_seals;
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size < 0 || static_cast<std::size_t>(status.st_size) != expected_size
        || (!read_only && !sealed)) {
        return false;
    }

    try {
        bytes.resize(expected_size);
    } catch (...) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < expected_size) {
        const auto received = pread(
            descriptor,
            bytes.data() + offset,
            expected_size - offset,
            static_cast<off_t>(offset));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            bytes.clear();
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

} // namespace mango_overlay::protocol
