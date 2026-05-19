#include "game/lobby_manager.hpp"

namespace opm::server {

LobbyManager::LobbyManager()
{
    lobbies_.push_back(Lobby {"default_lobby", 500U});
    lobbies_.push_back(Lobby {"race_lobby", 500U});
}

Lobby* LobbyManager::find(std::string_view name) noexcept
{
    for (auto& lobby : lobbies_) {
        if (lobby.name == name) {
            return &lobby;
        }
    }
    return nullptr;
}

bool LobbyManager::create(std::string_view name)
{
    // Check if lobby already exists
    if (find(name) != nullptr) {
        return false;
    }
    // Create new lobby with default capacity of 2
    lobbies_.push_back(Lobby {std::string(name), 2U});
    return true;
}

std::vector<std::size_t> LobbyManager::removeFromAll(socket_t fd)
{
    std::vector<std::size_t> affected;
    for (std::size_t i = 0; i < lobbies_.size(); ++i) {
        if (lobbies_[i].removeFd(fd)) {
            affected.push_back(i);
        }
    }
    return affected;
}

std::vector<opm::protocol::LobbyEntry> LobbyManager::listing() const
{
    std::vector<opm::protocol::LobbyEntry> entries;
    entries.reserve(lobbies_.size());
    for (const auto& lobby : lobbies_) {
        entries.push_back(opm::protocol::LobbyEntry {
            .name = lobby.name,
            .players = lobby.playerCount(),
            .capacity = lobby.capacity,
        });
    }
    return entries;
}

} // namespace opm::server
