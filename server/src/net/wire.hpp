#pragma once
#include "opm/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace opm::server {

inline constexpr std::size_t kProtocolHeaderSize = 9U;
inline constexpr std::array<std::uint8_t, 4> kHeaderPrefix {'O', 'P', 'M', 1U};

[[nodiscard]] std::uint32_t readU32LE(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept;

// Reusable wire-packet builder; reuses storage across ticks.
class WireBuilder {
public:
    void build(opm::protocol::MessageType type, std::span<const std::uint8_t> payload);
    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// Cursor-style packet view into a recv buffer. Caller owns the buffer storage.
struct PacketView {
    std::size_t start {0};
    std::size_t length {0};
};

// Scans `buffer` for fully-received protocol packets and emits views into `out`.
// Does not modify the buffer; caller is responsible for compacting after dispatch.
void scanPackets(std::span<const std::uint8_t> buffer, std::vector<PacketView>& out) noexcept;

} // namespace opm::server
