#include "server.hpp"

#include "net/sender.hpp"
#include "runtime/scoped_timer_resolution.hpp"
#include "runtime/scoped_wsa.hpp"
#include "runtime/tick_pacer.hpp"

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>

namespace opm::server {
namespace {

volatile std::sig_atomic_t gKeepRunning = 1;
void handleSignal(int) { gKeepRunning = 0; }

} // namespace

Server::Server(std::uint16_t port) noexcept : port_(port) {}

int Server::run()
{
    std::signal(SIGINT, handleSignal);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    ScopedWsa wsa;
    if (!wsa.ok()) {
        std::cerr << "[server] WSAStartup failed\n";
        return 1;
    }
    ScopedTimerResolution highRes;

    if (!setupListenSocket()) {
        return 1;
    }

    std::cout << "[server] custom TCP protocol online at port " << port_ << "\n";

    recvChunk_ = {};
    toDrop_.reserve(8);
    failedSends_.reserve(8);
    packetViews_.reserve(16);

    TickPacer pacer(std::chrono::microseconds(1'000'000 / kTickRateHz));

    while (gKeepRunning != 0) {
        tickAccept();
        tickRecvAndDispatch();
        tickProcessDrops();
        tickStepSimulation();
        tickBroadcastState();
        tickProcessSendFailures();
        tickReportStats();
        pacer.waitForNext();
    }

    shutdown();
    return 0;
}

bool Server::setupListenSocket()
{
    ScopedSocket sock(::socket(AF_INET, SOCK_STREAM, 0));
    if (!sock.valid()) {
        std::cerr << "[server] failed to create socket\n";
        return false;
    }

    int reuse = 1;
    ::setsockopt(sock.handle(), SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(sock.handle(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "[server] failed to bind port " << port_ << "\n";
        return false;
    }
    if (::listen(sock.handle(), 64) != 0) {
        std::cerr << "[server] failed to listen on port " << port_ << "\n";
        return false;
    }
    if (!setSocketNonBlocking(sock.handle())) {
        std::cerr << "[server] failed to set listen socket non-blocking\n";
        return false;
    }
    listenSocket_ = std::move(sock);
    return true;
}

void Server::shutdown()
{
    for (auto& [fd, conn] : connections_.map()) {
        (void)conn;
        closesocketCompat(fd);
    }
    listenSocket_.close();
    std::cout << "[server] shutdown\n";
}

void Server::tickAccept()
{
    while (true) {
        sockaddr_in clientAddr {};
        socklen_t clientLen = sizeof(clientAddr);
        const socket_t newFd = ::accept(listenSocket_.handle(),
            reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (newFd == kInvalidSocket) {
            return;
        }
        if (!setSocketNonBlocking(newFd)) {
            closesocketCompat(newFd);
            continue;
        }
        enableTcpNoDelay(newFd);
        connections_.add(newFd);
        std::cout << "[server] client connected fd=" << newFd << "\n";
    }
}

void Server::tickRecvAndDispatch()
{
    toDrop_.clear();
    for (auto& [fd, conn] : connections_.map()) {
        if (!conn.drainRecv(recvChunk_)) {
            toDrop_.push_back(fd);
            continue;
        }
        dispatchPackets(conn);
    }
}

void Server::dispatchPackets(ClientConnection& conn)
{
    auto& buffer = conn.recvBuffer();
    scanPackets(buffer, packetViews_);
    std::size_t consumed = 0;
    bool shouldDrop = false;

    for (const auto& view : packetViews_) {
        consumed = view.start + view.length;
        try {
            const auto type = static_cast<opm::protocol::MessageType>(buffer[view.start + 4U]);
            std::span<const std::uint8_t> payload(
                buffer.data() + view.start + kProtocolHeaderSize,
                view.length - kProtocolHeaderSize);
            if (!handleMessage(conn, type, payload)) {
                shouldDrop = true;
                break;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[server] dropped malformed packet from fd=" << conn.fd()
                      << " reason=" << ex.what() << "\n";
        }
    }

    if (consumed > 0) {
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    if (shouldDrop) {
        toDrop_.push_back(conn.fd());
    }
}

bool Server::handleMessage(ClientConnection& conn,
    opm::protocol::MessageType type,
    std::span<const std::uint8_t> payload)
{
    switch (type) {
        case opm::protocol::MessageType::LobbyListRequest:
            return handleLobbyListRequest(conn);
        case opm::protocol::MessageType::LobbyJoinRequest:
            return handleLobbyJoinRequest(conn, payload);
        case opm::protocol::MessageType::MovementInput:
            return handleMovementInput(conn, payload);
        case opm::protocol::MessageType::Ping:
            return handlePing(conn, payload);
        default:
            return true;
    }
}

bool Server::handleLobbyListRequest(ClientConnection& conn)
{
    const auto payloadBytes = opm::protocol::encodeLobbyListPayload(lobbies_.listing());
    scratchWire_.build(opm::protocol::MessageType::LobbyListResponse, payloadBytes);
    return conn.send(scratchWire_.view());
}

bool Server::handleLobbyJoinRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto requestedLobby = opm::protocol::decodeLobbyJoinRequestPayload(scratchPayload_);

    // Cleanly leave any existing lobby.
    lobbies_.removeFromAll(conn.fd());
    conn.session() = {};

    opm::protocol::LobbyJoinResponseData response;
    response.tickRateHz = kTickRateHz;
    response.lobbyName = requestedLobby;

    Lobby* lobby = lobbies_.find(requestedLobby);
    if (lobby == nullptr) {
        response.accepted = false;
        response.reason = "lobby_not_found";
        return sendJoinResponse(conn, response);
    }

    const auto assignedSlot = lobby->tryAssignSlot(conn.fd());
    if (!assignedSlot.has_value()) {
        response.accepted = false;
        response.reason = "lobby_full";
        return sendJoinResponse(conn, response);
    }

    response.accepted = true;
    response.playerIndex = *assignedSlot;
    response.reason = "joined";
    response.roster = lobby->roster();
    conn.session() = PeerSession {.lobbyName = lobby->name, .playerIndex = *assignedSlot};

    if (!sendJoinResponse(conn, response)) {
        return false;
    }
    if (!sendLevelSnapshot(conn)) {
        return false;
    }
    broadcastRoster(*lobby);
    return true;
}

bool Server::handleMovementInput(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    const auto& session = conn.session();
    if (session.playerIndex < pendingInputs_.size()) {
        scratchPayload_.assign(payload.begin(), payload.end());
        pendingInputs_[session.playerIndex] = opm::protocol::decodeMovementInputPayload(scratchPayload_);
    }
    return true;
}

bool Server::handlePing(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchWire_.build(opm::protocol::MessageType::Pong, payload);
    return conn.send(scratchWire_.view());
}

bool Server::sendJoinResponse(ClientConnection& conn, const opm::protocol::LobbyJoinResponseData& response)
{
    const auto payloadBytes = opm::protocol::encodeLobbyJoinResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::LobbyJoinResponse, payloadBytes);
    return conn.send(scratchWire_.view());
}

bool Server::sendLevelSnapshot(ClientConnection& conn)
{
    const auto payloadBytes = opm::protocol::encodeLevelSnapshotPayload(simulation_.level());
    scratchWire_.build(opm::protocol::MessageType::LevelSnapshot, payloadBytes);
    return conn.send(scratchWire_.view());
}

void Server::broadcastRoster(const Lobby& lobby)
{
    const auto roster = lobby.roster();
    const auto payloadBytes = opm::protocol::encodeRosterUpdatePayload(roster);
    WireBuilder wire;
    wire.build(opm::protocol::MessageType::RosterUpdate, payloadBytes);
    for (const auto fd : lobby.slots) {
        if (fd != kInvalidSocket) {
            (void)sendAll(fd, wire.view());
        }
    }
}

void Server::tickProcessDrops()
{
    for (const socket_t fd : toDrop_) {
        std::cout << "[server] client disconnected fd=" << fd << "\n";
        const auto affected = lobbies_.removeFromAll(fd);
        connections_.remove(fd);
        closesocketCompat(fd);
        for (const std::size_t lobbyIndex : affected) {
            broadcastRoster(lobbies_.at(lobbyIndex));
        }
    }
}

void Server::tickStepSimulation()
{
    for (auto& input : pendingInputs_) {
        input.frameIndex = simulation_.state().tick;
    }
    simulation_.step(pendingInputs_);
    for (auto& input : pendingInputs_) {
        input = {};
    }
}

void Server::tickBroadcastState()
{
    // Encode StateUpdate to wire bytes ONCE per tick, then broadcast raw.
    opm::protocol::StateUpdateData data;
    data.serverTick = simulation_.state().tick;
    for (std::size_t i = 0; i < data.players.size(); ++i) {
        const auto& s = simulation_.state().players[i];
        data.players[i] = opm::protocol::PlayerNetState {
            .positionX = s.position.x,
            .positionY = s.position.y,
            .velocityX = s.velocity.x,
            .velocityY = s.velocity.y,
            .onGround = s.onGround,
            .facingRight = s.facingRight,
            .skidding = s.skidding,
            .crouching = s.crouching,
            .pSpeedActive = s.pSpeedActive,
            .pSpeedMeter = s.pSpeedMeter,
        };
    }
    const auto payload = opm::protocol::encodeStateUpdatePayload(data);
    broadcastWire_.build(opm::protocol::MessageType::StateUpdate, payload);

    failedSends_.clear();
    for (auto& [fd, conn] : connections_.map()) {
        if (conn.session().playerIndex == kInvalidPlayerSlot) {
            continue; // Not in a lobby yet.
        }
        if (!conn.send(broadcastWire_.view())) {
            failedSends_.push_back(fd);
        }
    }
}

void Server::tickProcessSendFailures()
{
    for (const socket_t fd : failedSends_) {
        std::cout << "[server] client send failure fd=" << fd << "\n";
        const auto affected = lobbies_.removeFromAll(fd);
        connections_.remove(fd);
        closesocketCompat(fd);
        for (const std::size_t lobbyIndex : affected) {
            broadcastRoster(lobbies_.at(lobbyIndex));
        }
    }
}

void Server::tickReportStats()
{
    if (statsAnchor_.time_since_epoch().count() == 0) {
        statsAnchor_ = std::chrono::steady_clock::now();
        statsAnchorTick_ = simulation_.state().tick;
        return;
    }
    if (simulation_.state().tick % 300U == 0U) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - statsAnchor_).count();
        const auto ticksSince = simulation_.state().tick - statsAnchorTick_;
        const double measuredHz = (elapsedMs > 0)
            ? (static_cast<double>(ticksSince) * 1000.0 / static_cast<double>(elapsedMs))
            : 0.0;
        std::cout << "[server] tick=" << simulation_.state().tick
                  << " hash=" << simulation_.stateHash()
                  << " measuredHz=" << measuredHz << "\n";
        statsAnchor_ = now;
        statsAnchorTick_ = simulation_.state().tick;
    }
}

} // namespace opm::server
