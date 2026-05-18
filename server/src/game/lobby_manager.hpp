#pragma once
#include "game/lobby.hpp"
#include "net/socket_compat.hpp"
#include "opm/protocol.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace opm::server {

// Owns the set of lobbies the server hosts.
class LobbyManager {
public:
    LobbyManager();

    [[nodiscard]] Lobby* find(std::string_view name) noexcept;
    [[nodiscard]] Lobby& at(std::size_t i) noexcept { return lobbies_[i]; }
    [[nodiscard]] auto& all() noexcept { return lobbies_; }

    // Creates a new lobby with the given name. Returns true if successful, false if a lobby with that name already exists.
    [[nodiscard]] bool create(std::string_view name);

    // Returns indices of lobbies whose membership changed.
    std::vector<std::size_t> removeFromAll(socket_t fd);

    [[nodiscard]] std::vector<opm::protocol::LobbyEntry> listing() const;

private:
    std::vector<Lobby> lobbies_;
};

} // namespace opm::server
