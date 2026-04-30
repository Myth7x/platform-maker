#pragma once
#include <cstdint>
#include <string>

namespace opm::server {

inline constexpr std::uint8_t kInvalidPlayerSlot = 0xFFU;

// Per-connection lobby membership. Default-constructed = "not in any lobby".
struct PeerSession {
    std::string lobbyName;
    std::uint8_t playerIndex {kInvalidPlayerSlot};
};

} // namespace opm::server
