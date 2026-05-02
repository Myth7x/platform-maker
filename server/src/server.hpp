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
#include <vector>

namespace opm::server {

inline constexpr std::uint32_t kTickRateHz = 60U;

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
    void tickBroadcastState();
    void tickProcessSendFailures();
    void tickReportStats();

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
};

} // namespace opm::server
