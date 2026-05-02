#pragma once
#include "net/socket_compat.hpp"
#include "opm/protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opm::server {

// A single lobby slot table. Owns the slot fds (referencing live
// connections) but does not manage their lifetime — Server is responsible
// for closing sockets.
class Lobby {
public:
    std::string name;
    std::uint32_t capacity {2};
    std::vector<socket_t> slots {};

    // Tries to assign `fd` to the first free slot. Returns the slot index, or
    // nullopt if the lobby is full.
    [[nodiscard]] std::optional<std::uint16_t> tryAssignSlot(socket_t fd) noexcept;

    // Removes `fd` from any slot it occupies. Returns true if any slot changed.
    bool removeFd(socket_t fd) noexcept;

    [[nodiscard]] std::uint32_t playerCount() const noexcept;

    // Builds a roster snapshot for inclusion in protocol responses.
    [[nodiscard]] std::vector<opm::protocol::PlayerInfo> roster() const;
};

} // namespace opm::server
