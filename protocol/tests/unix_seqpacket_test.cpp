#include "mango_overlay/protocol/packet.hpp"
#include "mango_overlay/protocol/unix_seqpacket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <vector>

using namespace mango_overlay::protocol;

namespace {

std::vector<std::uint8_t> packet_with_flags(std::uint16_t flags)
{
    const std::array<std::uint8_t, 3> payload { 1, 2, 3 };
    return encode_packet(
        PacketHeader { ProtocolVersion { 1, 0 }, MessageType::upload_resource, flags, 7 },
        ByteView { payload.data(), payload.size() });
}

} // namespace

int main()
{
    std::array<int, 2> sockets {};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) != 0) {
        std::perror("socketpair");
        return 1;
    }

    const std::vector<std::uint8_t> contents { 4, 8, 15, 16, 23, 42 };
    auto resource = make_sealed_memfd(ByteView { contents.data(), contents.size() });
    const auto attached_packet = packet_with_flags(packet_flag_file_descriptor);
    if (!resource
        || send_seqpacket(
               sockets[0],
               ByteView { attached_packet.data(), attached_packet.size() },
               resource.get())
            != SeqpacketResult::accepted) {
        std::fputs("could not send a sealed resource descriptor\n", stderr);
        return 1;
    }

    std::array<std::uint8_t, 4096> received {};
    std::size_t received_size = 0;
    UniqueFileDescriptor attachment;
    if (receive_seqpacket(
            sockets[1], received.data(), received.size(), received_size, attachment)
            != SeqpacketResult::accepted
        || received_size != attached_packet.size() || !attachment) {
        std::fputs("did not receive exactly one resource descriptor\n", stderr);
        return 1;
    }
    std::vector<std::uint8_t> copied;
    if (!read_resource_descriptor(
            attachment.get(), contents.size(), contents.size(), copied)
        || copied != contents) {
        std::fputs("resource descriptor contents were not copied exactly\n", stderr);
        return 1;
    }

    const auto inline_packet = packet_with_flags(0);
    if (send_seqpacket(
            sockets[0], ByteView { inline_packet.data(), inline_packet.size() })
            != SeqpacketResult::accepted
        || receive_seqpacket(
               sockets[1], received.data(), received.size(), received_size, attachment)
            != SeqpacketResult::accepted
        || attachment) {
        std::fputs("inline seqpacket unexpectedly carried a descriptor\n", stderr);
        return 1;
    }

    if (send_seqpacket(
            sockets[0], ByteView { attached_packet.data(), attached_packet.size() })
            != SeqpacketResult::rejected
        || send_seqpacket(
               sockets[0],
               ByteView { inline_packet.data(), inline_packet.size() },
               resource.get())
            != SeqpacketResult::rejected) {
        std::fputs("packet flags and descriptor presence were not enforced\n", stderr);
        return 1;
    }

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
