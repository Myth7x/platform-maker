#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "net/message_router.hpp"
#include "net/socket_compat.hpp"
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
    std::uint16_t playerIndex {0xFFFFU};
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
    std::vector<std::uint16_t> background {};
    std::vector<std::uint16_t> foliage {};
    std::vector<std::uint16_t> foreground {};
};

struct RemotePlayerState {
    // Server-side simulation slot this state belongs to. The wire format
    // is sparse — only active slots are sent — so use this rather than the
    // index in StateUpdate::players when matching states to local Actors.
    std::uint16_t slotIndex {0};
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
    std::uint8_t style {0};
    std::uint8_t powerupTransitionFrames {0};
    std::uint8_t invincibilityFrames {0};
};

struct RemoteActorState {
    float positionX {0.0F};
    float positionY {0.0F};
    float velocityX {0.0F};
    float velocityY {0.0F};
    bool alive {true};
    bool facingRight {true};
    std::uint8_t script {0};
    std::uint8_t enemyKind {0};
    std::uint8_t category {0};
};

struct StateUpdate {
    std::uint32_t serverTick {0};
    std::vector<RemotePlayerState> players {};
    std::vector<RemoteActorState> actors {};
    // Mirrored from protocol::StateUpdateData tail.
    opm::protocol::GamePhase phase {opm::protocol::GamePhase::PreGame};
    std::uint32_t countdownTicks {0};
    std::uint16_t winnerSlot {0xFFFFU};
    std::string selectedMap {};
    bool selectedTiebreak {false};
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
    [[nodiscard]] bool requestLogin(const std::string& username, const std::string& password,
        std::uint32_t timeoutMs, opm::protocol::LoginResponseData& response, std::string& status);
    [[nodiscard]] bool requestUpdateProfile(const std::string& displayName,
        std::uint32_t timeoutMs, std::string& status);
    [[nodiscard]] bool sendMovementInput(const opm::engine::InputFrame& input, std::string& status);
    [[nodiscard]] bool pollStateUpdate(std::uint32_t timeoutMs, StateUpdate& update, std::string& status);
    void drainRosterUpdates(std::vector<std::vector<opm::protocol::PlayerInfo>>& out);
    // Sends a Ping if `intervalMs` has elapsed since the last one. Cheap; safe to call every frame.
    void sendPingIfDue(std::uint32_t intervalMs);

    // Level catalogue / persistence (request-reply against the server).
    [[nodiscard]] bool requestLevelList(std::uint32_t timeoutMs, std::vector<std::string>& names, std::string& status);
    [[nodiscard]] bool requestLoadLevel(const std::string& name, std::uint32_t timeoutMs,
        opm::engine::LevelData& level, std::string& status);
    [[nodiscard]] bool requestSaveLevel(const std::string& name, const opm::engine::LevelData& level,
        std::uint32_t timeoutMs, std::string& status);

    // Asks the server to switch the lobby simulation to a stored level.
    // Returns true when the server accepted the change. The new level data is
    // delivered separately as a LevelSnapshot broadcast — drain it via
    // drainLevelSnapshots() to receive it.
    [[nodiscard]] bool requestSetLobbyLevel(const std::string& levelName,
        std::uint32_t timeoutMs, std::string& status);

    // Cast / withdraw a map vote during the lobby PreGame countdown.
    // Empty levelName withdraws this player's vote. Fire-and-forget;
    // server broadcasts the tally back via MapVoteUpdate.
    [[nodiscard]] bool sendMapVote(const std::string& levelName, std::string& status);

    // Send LeaveLobby message (fire-and-forget).
    [[nodiscard]] bool sendLeaveLobby(std::string& status);

    // Request to create a new lobby.
    [[nodiscard]] bool requestCreateLobby(const std::string& lobbyName,
        std::uint32_t timeoutMs, std::string& status);

    // Returns and clears any LevelSnapshot messages received since the last
    // call (during gameplay polls).
    void drainLevelSnapshots(std::vector<LevelSnapshot>& out);
    [[nodiscard]] std::uint32_t getPingMs() const;
    [[nodiscard]] bool isConnected() const;
    void disconnect();

    // Owned router for side-channel messages (RosterUpdate /
    // LevelSnapshot / Pong). Exposed so screens can drain queued
    // updates and read the smoothed ping.
    [[nodiscard]] MessageRouter& router() noexcept { return router_; }
    [[nodiscard]] const MessageRouter& router() const noexcept { return router_; }

private:
    // Reads up to one chunk into recvBuffer_, blocking until either data
    // arrives, the timeout expires, or the peer closes. Returns false (and
    // sets `status`) on timeout / closed peer / receive error.
    [[nodiscard]] bool pumpRecv(std::uint32_t timeoutMs, std::string& status);

    [[nodiscard]] bool awaitMessage(opm::protocol::MessageType expected,
        std::uint32_t timeoutMs,
        opm::protocol::Message& out,
        std::string& status);

    socket_t fd_ {kInvalidSocket};
    std::vector<std::uint8_t> recvBuffer_ {};
    std::chrono::steady_clock::time_point lastPingSentAt_ {};
    MessageRouter router_ {};
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
