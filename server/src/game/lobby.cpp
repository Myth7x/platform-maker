#include "game/lobby.hpp"

#include <string_view>
#include <utility>

namespace opm::server {

std::optional<std::uint8_t> Lobby::tryAssignSlot(socket_t fd) noexcept
{
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] == kInvalidSocket) {
            slots[i] = fd;
            return static_cast<std::uint8_t>(i);
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
    static constexpr std::array<std::string_view, 2> names {"Player 1", "Player 2"};
    static constexpr std::array<std::array<std::uint8_t, 3>, 2> colors {{
        {224U, 60U, 60U},
        {70U, 190U, 80U},
    }};
    std::vector<opm::protocol::PlayerInfo> out;
    out.reserve(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        opm::protocol::PlayerInfo info;
        info.playerIndex = static_cast<std::uint8_t>(i);
        info.connected = slots[i] != kInvalidSocket;
        info.colorR = colors[i][0];
        info.colorG = colors[i][1];
        info.colorB = colors[i][2];
        info.displayName = std::string(names[i]);
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace opm::server
