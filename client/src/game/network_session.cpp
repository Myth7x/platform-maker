#include "game/network_session.hpp"

#include "opm/protocol.hpp"

#include <charconv>
#include <cstddef>
#include <iostream>
#include <system_error>
#include <vector>

namespace opm::client::game {

bool parseAddress(std::string_view input, std::string& hostOut, std::uint16_t& portOut)
{
    const auto colon = input.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= input.size()) {
        return false;
    }
    hostOut.assign(input.substr(0, colon));
    const auto portStr = input.substr(colon + 1);
    int port = 0;
    const auto* begin = portStr.data();
    const auto* end = portStr.data() + portStr.size();
    auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc {} || ptr != end || port <= 0 || port > 65535) {
        return false;
    }
    portOut = static_cast<std::uint16_t>(port);
    return true;
}

opm::engine::LevelData levelFromSnapshot(const opm::client::net::LevelSnapshot& snapshot)
{
    opm::engine::LevelData level;
    auto setLayer = [&](opm::engine::TileLayer& layer, const std::vector<std::uint16_t>& src) {
        layer.width = snapshot.width;
        layer.height = snapshot.height;
        const auto expected = static_cast<std::size_t>(snapshot.width) * static_cast<std::size_t>(snapshot.height);
        if (src.size() == expected) {
            layer.tileIndices = src;
        } else {
            // Tolerate older servers that didn't send a layer.
            layer.tileIndices.assign(expected, 0U);
        }
    };
    setLayer(level.background, snapshot.background);
    setLayer(level.foliage,    snapshot.foliage);
    setLayer(level.foreground, snapshot.foreground);
    level.spawnX = snapshot.spawnX;
    level.spawnY = snapshot.spawnY;
    level.goalX = snapshot.goalX;
    level.goalY = snapshot.goalY;
    return level;
}

ConnectResult tryConnect(NetworkSessionContext& net, const std::string& host, std::uint16_t port)
{
    if (!net.session) {
        net.session = std::make_unique<opm::client::net::SessionClient>();
    }
    std::string status;
    if (!net.session->connect(host, port, 1000U, status)) {
        return {false, "connect failed: " + status};
    }
    return {true, "connected"};
}

ConnectResult fetchLobbyList(NetworkSessionContext& net,
                             const std::string& host, std::uint16_t port,
                             std::vector<LobbyListing>& out)
{
    if (!net.session) {
        net.session = std::make_unique<opm::client::net::SessionClient>();
    }
    out.clear();

    std::string status;
    if (!net.session->isConnected()) {
        if (!net.session->connect(host, port, 1000U, status)) {
            std::cout << "[client] connect status: " << status << "\n";
            return {false, "connect failed: " + status};
        }
    }

    std::vector<opm::client::net::LobbyInfo> lobbies;
    if (!net.session->requestLobbyList(1000U, lobbies, status)) {
        std::cout << "[client] lobby request status: " << status << "\n";
        return {false, "lobby list failed: " + status};
    }
    out.reserve(lobbies.size());
    for (const auto& l : lobbies) {
        out.push_back(LobbyListing {.name = l.name, .players = l.players, .capacity = l.capacity});
    }
    std::cout << "[client] received " << out.size() << " lobby entries\n";
    return {true, "ok"};
}

ConnectResult joinNamedLobby(NetworkSessionContext& net,
                             const std::string& host, std::uint16_t port,
                             const std::string& lobbyName)
{
    if (!net.session) {
        net.session = std::make_unique<opm::client::net::SessionClient>();
    }
    net.connected = false;
    net.actors.resetLocalOnly();

    std::string status;
    if (!net.session->isConnected()) {
        if (!net.session->connect(host, port, 1000U, status)) {
            std::cout << "[client] connect status: " << status << "\n";
            return {false, "connect failed: " + status};
        }
    }

    opm::client::net::JoinResult joinResult;
    const bool joined = net.session->joinLobby(lobbyName, 1000U, joinResult, status);
    std::cout << "[client] join lobby='" << lobbyName << "' status=" << status
              << " success=" << (joined ? "true" : "false")
              << " player=" << static_cast<int>(joinResult.playerIndex)
              << " tickHz=" << joinResult.tickRateHz << "\n";

    if (!joined) {
        return {false, "join refused: " + status};
    }

    net.localPlayerIndex = joinResult.playerIndex;
    net.actors.bindLocalToServer(joinResult.playerIndex);
    net.actors.applyRoster(joinResult.roster, joinResult.playerIndex);

    opm::client::net::LevelSnapshot snapshot;
    if (net.session->receiveLevelSnapshot(1000U, snapshot, status)) {
        net.networkLevel = levelFromSnapshot(snapshot);
        opm::engine::PlayerState localSpawnState;
        localSpawnState.position.x = snapshot.spawnX;
        localSpawnState.position.y = snapshot.spawnY;
        net.actors.updateLocalState(localSpawnState);
        std::cout << "[client] level snapshot: " << snapshot.width << "x" << snapshot.height
                  << " tiles=" << snapshot.foliage.size() << " spawn=(" << snapshot.spawnX << "," << snapshot.spawnY
                  << ")\n";
    } else {
        return {false, "snapshot failed: " + status};
    }

    opm::client::net::StateUpdate update;
    if (net.session->pollStateUpdate(1000U, update, status)) {
        net.actors.applyStateUpdate(update, net.localPlayerIndex);
    }

    net.connected = true;
    std::cout << "[client] network session active host=" << host << " port=" << port
              << " lobby=" << lobbyName << "\n";
    return {true, "connected"};
}

} // namespace opm::client::game
