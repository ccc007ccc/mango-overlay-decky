#pragma once

#include "mango_overlay/protocol/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mango_overlay::protocol {

class UniqueFileDescriptor {
public:
    UniqueFileDescriptor() = default;
    explicit UniqueFileDescriptor(int descriptor) noexcept;
    ~UniqueFileDescriptor();

    UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept;
    UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept;
    UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;

    int get() const noexcept { return descriptor_; }
    explicit operator bool() const noexcept { return descriptor_ >= 0; }
    int release() noexcept;
    void reset(int descriptor = -1) noexcept;

private:
    int descriptor_ = -1;
};

enum class SeqpacketResult {
    accepted,
    peer_closed,
    io_error,
    rejected,
};

SeqpacketResult send_seqpacket(
    int socket_fd,
    ByteView packet,
    int attachment_fd = -1);

SeqpacketResult receive_seqpacket(
    int socket_fd,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t& received_size,
    UniqueFileDescriptor& attachment);

UniqueFileDescriptor make_sealed_memfd(ByteView bytes);

bool read_resource_descriptor(
    int descriptor,
    std::size_t expected_size,
    std::size_t maximum_size,
    std::vector<std::uint8_t>& bytes);

} // namespace mango_overlay::protocol
