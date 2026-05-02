#pragma once
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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace opm::server {

inline constexpr std::uint32_t kTickRateHz = 60U;

// Game-phase timing constants. Tunable from one place.
inline constexpr std::uint32_t kPreGameCountdownTicks  = kTickRateHz * 5U;  // 5 seconds before game starts
inline constexpr std::uint32_t kGameOverDisplayTicks   = kTickRateHz * 6U;  // 6 seconds of winner display before reset
inline constexpr std::uint16_t kNoWinnerSlot           = 0xFFFFU;

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
    void enterPlaying();
    void enterGameOver(std::uint16_t winnerSlot);
    void confinePlayersToSafeZone();
    [[nodiscard]] std::uint16_t firstPlayerAtGoal() const;
    void broadcastMapVoteUpdate();
    [[nodiscard]] std::string tallyWinningMap() const;

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

    void broadcastLevelSnapshotToLobby(const Lobby& lobby);

    [[nodiscard]] bool sendJoinResponse(ClientConnection& conn, const opm::protocol::LobbyJoinResponseData& response);
    [[nodiscard]] bool sendLevelSnapshot(ClientConnection& conn);
    void broadcastRoster(const Lobby& lobby);

    // ---- members ----
    std::uint16_t port_;
    ScopedSocket listenSocket_;
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
};

} // namespace opm::server
