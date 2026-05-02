#include "game/lobby.hpp"

#include <string>
#include <utility>

namespace opm::server {

std::optional<std::uint16_t> Lobby::tryAssignSlot(socket_t fd) noexcept
{
    if (slots.size() != static_cast<std::size_t>(capacity)) {
        slots.resize(static_cast<std::size_t>(capacity), kInvalidSocket);
    }

    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] == kInvalidSocket) {
            slots[i] = fd;
            return static_cast<std::uint16_t>(i);
        }
    }
    return std::nullopt;
}

bool Lobby::removeFd(socket_t fd) noexcept
{
    bool changed = false;
    for (auto& slot : slots) {
        if (slot == fd) {
            slot = kInvalidSocket;
            changed = true;
        }
    }
    return changed;
}

std::uint32_t Lobby::playerCount() const noexcept
{
    std::uint32_t count = 0;
    for (const auto fd : slots) {
        if (fd != kInvalidSocket) {
            ++count;
        }
    }
    return count;
}

std::vector<opm::protocol::PlayerInfo> Lobby::roster() const
{
    std::vector<opm::protocol::PlayerInfo> out;
    out.reserve(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        opm::protocol::PlayerInfo info;
        info.playerIndex = static_cast<std::uint16_t>(i);
        info.connected = slots[i] != kInvalidSocket;
        info.colorR = static_cast<std::uint8_t>((37U * static_cast<std::uint32_t>(i) + 64U) % 256U);
        info.colorG = static_cast<std::uint8_t>((59U * static_cast<std::uint32_t>(i) + 96U) % 256U);
        info.colorB = static_cast<std::uint8_t>((83U * static_cast<std::uint32_t>(i) + 128U) % 256U);
        info.displayName = "Player " + std::to_string(i + 1U);
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace opm::server
