#include "net/wire.hpp"

namespace opm::server {

std::uint32_t readU32LE(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void WireBuilder::build(opm::protocol::MessageType type, std::span<const std::uint8_t> payload)
{
    bytes_.clear();
    bytes_.reserve(kProtocolHeaderSize + payload.size());
    bytes_.insert(bytes_.end(), kHeaderPrefix.begin(), kHeaderPrefix.end());
    bytes_.push_back(static_cast<std::uint8_t>(type));
    const auto size = static_cast<std::uint32_t>(payload.size());
    bytes_.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    bytes_.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
    bytes_.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xFFU));
    bytes_.push_back(static_cast<std::uint8_t>((size >> 24U) & 0xFFU));
    bytes_.insert(bytes_.end(), payload.begin(), payload.end());
}

void scanPackets(std::span<const std::uint8_t> buffer, std::vector<PacketView>& out) noexcept
{
    out.clear();
    std::size_t cursor = 0;
    while (buffer.size() - cursor >= kProtocolHeaderSize) {
        const std::uint32_t payloadSize = readU32LE(buffer, cursor + 5U);
        const std::size_t packetSize = kProtocolHeaderSize + static_cast<std::size_t>(payloadSize);
        if (buffer.size() - cursor < packetSize) {
            break;
        }
        out.push_back(PacketView {.start = cursor, .length = packetSize});
        cursor += packetSize;
    }
}

} // namespace opm::server
