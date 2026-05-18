#pragma once
#include <cstdint>
#include <string>

namespace opm::server {

inline constexpr std::uint16_t kInvalidPlayerSlot = 0xFFFFU;

// Per-connection lobby membership. Default-constructed = "not in any lobby".
struct PeerSession {
    std::string lobbyName;
    std::uint16_t playerIndex {kInvalidPlayerSlot};
    std::string authToken {};          // authentication token (empty = not authenticated)
    std::string username {};           // authenticated username
    std::string displayName {};        // player's display name
};

} // namespace opm::server
