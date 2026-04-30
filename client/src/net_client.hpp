#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <vector>

#include "opm/engine.hpp"
#include "opm/protocol.hpp"

namespace opm::client::net {

struct LobbyInfo {
    std::string name {};
    std::uint32_t players {0};
    std::uint32_t capacity {0};
};

struct JoinResult {
    bool accepted {false};
    std::uint8_t playerIndex {0xFFU};
    std::uint32_t tickRateHz {0};
    std::string lobbyName {};
    std::string reason {};
    std::vector<opm::protocol::PlayerInfo> roster {};
};

struct LevelSnapshot {
    std::uint32_t width {0};
    std::uint32_t height {0};
    float spawnX {0.0F};
    float spawnY {0.0F};
    float goalX {0.0F};
    float goalY {0.0F};
    std::vector<std::uint16_t> tiles {};
};

struct RemotePlayerState {
    float positionX {0.0F};
    float positionY {0.0F};
    float velocityX {0.0F};
    float velocityY {0.0F};
    bool onGround {true};
    bool facingRight {true};
    bool skidding {false};
    bool crouching {false};
    bool pSpeedActive {false};
    float pSpeedMeter {0.0F};
};

struct StateUpdate {
    std::uint32_t serverTick {0};
    std::array<RemotePlayerState, 2> players {};
};

class SessionClient {
public:
    SessionClient();
    ~SessionClient();

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port, std::uint32_t timeoutMs, std::string& status);
    [[nodiscard]] bool requestLobbyList(std::uint32_t timeoutMs, std::vector<LobbyInfo>& lobbies, std::string& status);
    [[nodiscard]] bool joinLobby(const std::string& lobbyName, std::uint32_t timeoutMs, JoinResult& result, std::string& status);
    [[nodiscard]] bool receiveLevelSnapshot(std::uint32_t timeoutMs, LevelSnapshot& snapshot, std::string& status);
    [[nodiscard]] bool sendMovementInput(const opm::engine::InputFrame& input, std::string& status);
    [[nodiscard]] bool pollStateUpdate(std::uint32_t timeoutMs, StateUpdate& update, std::string& status);
    [[nodiscard]] std::uint32_t getPingMs() const;
    [[nodiscard]] bool isConnected() const;
    void disconnect();

private:
    struct Impl;
    Impl* impl_;
};

[[nodiscard]] std::vector<LobbyInfo> requestLobbyList(
    const std::string& host,
    std::uint16_t port,
    std::uint32_t timeoutMs,
    std::string& status
);

[[nodiscard]] bool joinLobby(
    const std::string& host,
    std::uint16_t port,
    const std::string& lobbyName,
    std::uint32_t timeoutMs,
    JoinResult* joinResult,
    std::string& status
);

[[nodiscard]] inline bool joinLobby(
    const std::string& host,
    std::uint16_t port,
    const std::string& lobbyName,
    std::uint32_t timeoutMs,
    std::string& status
)
{
    return joinLobby(host, port, lobbyName, timeoutMs, nullptr, status);
}

} // namespace opm::client::net
