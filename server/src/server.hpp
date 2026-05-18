#pragma once
#include "game/account_manager.hpp"
#include "game/level_storage.hpp"
#include "game/lobby_manager.hpp"
#include "net/connection_table.hpp"
#include "net/scoped_socket.hpp"
#include "net/wire.hpp"
#include "opm/engine.hpp"
#include "opm/protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace opm::server {

inline constexpr std::uint32_t kTickRateHz = 60U;

// Game-phase timing constants. Tunable from one place.
// Voting countdown: starts at 60s when the first vote is cast; clamps
// down to 15s once every active player has voted.
inline constexpr std::uint32_t kVoteStartCountdownTicks = kTickRateHz * 60U;
inline constexpr std::uint32_t kAllVotedCountdownTicks  = kTickRateHz * 15U;
// After voting ends the server picks the winning map (random pick if
// the top vote is tied) and holds in PreGame for this many ticks so
// every client can show a "selected: X" announcement before the
// gameplay screen takes over.
inline constexpr std::uint32_t kAnnounceTicks           = kTickRateHz * 3U;
// After clients have transitioned into the gameplay screen, the server
// holds in RoundStarting for this many ticks. Players spawn but stay
// confined to the spawn safezone, with a "Round starts in 5..." HUD.
inline constexpr std::uint32_t kRoundStartCountdownTicks = kTickRateHz * 5U;
inline constexpr std::uint32_t kGameOverDisplayTicks    = kTickRateHz * 5U;  // winner display
inline constexpr std::uint16_t kNoWinnerSlot            = 0xFFFFU;

// Top-level orchestrator. Owns the listen socket, lobbies, connection table,
// engine simulation and the per-tick pacer. Drives the fixed-rate tick loop
// and dispatches incoming protocol messages.
class Server {
public:
    explicit Server(std::uint16_t port);

    int run();

private:
    [[nodiscard]] bool setupListenSocket();
    void shutdown();

    // ---- tick stages ----
    void tickAccept();
    void tickRecvAndDispatch();
    void tickProcessDrops();
    void tickStepSimulation();
    void tickGamePhase();
    void tickBroadcastState();
    void tickProcessSendFailures();
    void tickReportStats();

    // ---- game-phase helpers ----
    void enterPreGame();
    void enterRoundStarting();   // transitions from PreGame announce to gameplay countdown
    void enterPlaying();         // transitions from RoundStarting to actual race
    void enterGameOver(std::uint16_t winnerSlot);
    void confinePlayersToSafeZone();
    [[nodiscard]] std::uint16_t firstPlayerAtGoal() const;
    void broadcastMapVoteUpdate();
    // Picks the winning map (with random tiebreak across the top tier)
    // and stores it in selectedMap_ + selectedTiebreak_. No-op if no
    // votes have been cast — selectedMap_ stays empty and the current
    // server level is reused.
    void pickWinningMap();

    void dispatchPackets(ClientConnection& conn);

    // Returns false to request the connection be dropped.
    [[nodiscard]] bool handleMessage(ClientConnection& conn,
        opm::protocol::MessageType type,
        std::span<const std::uint8_t> payload);

    [[nodiscard]] bool handleLobbyListRequest(ClientConnection& conn);
    [[nodiscard]] bool handleLobbyJoinRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleMovementInput(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handlePing(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleLevelListRequest(ClientConnection& conn);
    [[nodiscard]] bool handleLevelLoadRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleLevelSaveRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleLobbySetLevelRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleMapVoteRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleLoginRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);
    [[nodiscard]] bool handleUpdateProfileRequest(ClientConnection& conn, std::span<const std::uint8_t> payload);

    void broadcastLevelSnapshotToLobby(const Lobby& lobby);

    [[nodiscard]] bool sendJoinResponse(ClientConnection& conn, const opm::protocol::LobbyJoinResponseData& response);
    [[nodiscard]] bool sendLevelSnapshot(ClientConnection& conn);
    void broadcastRoster(const Lobby& lobby);

    // ---- members ----
    std::uint16_t port_;
    ScopedSocket listenSocket_;
    AccountManager accountManager_;
    LobbyManager lobbies_;
    ConnectionTable connections_;
    LevelStorage levelStorage_;
    opm::engine::Simulation simulation_;
    std::vector<opm::engine::InputFrame> pendingInputs_ {};

    // Reusable per-tick scratch — hoisted to avoid reallocating each tick.
    std::array<std::uint8_t, kRecvChunkSize> recvChunk_ {};
    std::vector<socket_t> toDrop_;
    std::vector<socket_t> failedSends_;
    std::vector<PacketView> packetViews_;
    std::vector<std::uint8_t> scratchPayload_;
    WireBuilder scratchWire_;
    WireBuilder broadcastWire_;

    // Self-measurement of tick rate.
    std::chrono::steady_clock::time_point statsAnchor_ {};
    std::uint32_t statsAnchorTick_ {0};

    // ---- game-phase state ----
    // Single shared phase across the whole server (one simulation, one
    // active level — see lobby_manager.cpp). When the first player joins
    // we enter PreGame with a countdown; on countdown elapse we apply
    // the most-voted level and switch to Playing; first-to-goal wins.
    opm::protocol::GamePhase gamePhase_ {opm::protocol::GamePhase::PreGame};
    std::uint32_t            countdownTicks_ {0};
    std::uint16_t            winnerSlot_ {kNoWinnerSlot};
    // Map vote ballots, keyed by player slot index.
    std::unordered_map<std::uint16_t, std::string> mapVotes_ {};
    // Levels available for voting (refreshed lazily from levelStorage_).
    std::vector<std::string> availableLevels_ {};
    // The post-vote winning map. Empty during the voting sub-phase;
    // set during the announce sub-phase. enterPlaying clears it.
    std::string selectedMap_ {};
    bool        selectedTiebreak_ {false};
    // RNG for breaking vote ties.
    std::mt19937 rng_ {std::random_device {}()};
};

} // namespace opm::server
