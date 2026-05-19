#include "server.hpp"

#include "net/sender.hpp"
#include "runtime/scoped_timer_resolution.hpp"
#include "runtime/scoped_wsa.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>

namespace opm::server {
namespace {

volatile std::sig_atomic_t gKeepRunning = 1;
void handleSignal(int) { gKeepRunning = 0; }

} // namespace

Server::Server(std::uint16_t port)
    : port_(port)
    , accountManager_("accounts.json")
    , levelStorage_(std::filesystem::current_path() / "levels")
{
    constexpr std::size_t kMaxLobbyPlayers = 500U;
    simulation_.setPlayerCount(kMaxLobbyPlayers);
    pendingInputs_.resize(kMaxLobbyPlayers);
    // No connected players yet — deactivate every slot so unused slots don't
    // pile up at the spawn AABB and block real players.
    for (std::size_t i = 0; i < kMaxLobbyPlayers; ++i) {
        simulation_.setPlayerActive(i, false);
    }
}

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

    // Initialize account manager
    if (!accountManager_.initialize()) {
        std::cerr << "[server] failed to initialize account manager\n";
        return 1;
    }

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
        tickGamePhase();
        tickBroadcastState();
        tickProcessSendFailures();
        tickReportStats();
        idleIO(pacer.deadline());
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

void Server::idleIO(const std::chrono::steady_clock::time_point tickDeadline)
{
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= tickDeadline) {
            break;
        }
        const auto remainUs =
            std::chrono::duration_cast<std::chrono::microseconds>(tickDeadline - now).count();
        // Cap to at least 0 ms to avoid blocking past the deadline.
        const int timeoutMs = static_cast<int>(std::max<std::int64_t>(remainUs / 1000, 0));

        idlePollFds_.clear();
        idlePollFds_.push_back({listenSocket_.handle(), POLLIN, 0});
        for (const auto& [fd, _conn] : connections_.map()) {
            idlePollFds_.push_back({fd, POLLIN, 0});
        }

        const int n = sockPoll(idlePollFds_.data(),
#ifdef _WIN32
            static_cast<ULONG>(idlePollFds_.size()),
#else
            static_cast<nfds_t>(idlePollFds_.size()),
#endif
            timeoutMs);
        if (n <= 0) {
            // Timeout or error — deadline reached or no data.
            break;
        }

        // Data is available on at least one fd — do a quick recv+dispatch
        // pass so Pings (and any other messages) are handled immediately.
        tickAccept();
        tickRecvAndDispatch();
        tickProcessDrops();
    }
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
    const std::size_t offset = conn.recvOffset();
    // Scan only the unconsumed tail of the buffer.
    std::span<const std::uint8_t> unread(buffer.data() + offset, buffer.size() - offset);
    scanPackets(unread, packetViews_);
    std::size_t consumed = 0;
    bool shouldDrop = false;

    for (const auto& view : packetViews_) {
        consumed = view.start + view.length;
        try {
            const auto type = static_cast<opm::protocol::MessageType>(
                buffer[offset + view.start + 4U]);
            std::span<const std::uint8_t> payload(
                buffer.data() + offset + view.start + kProtocolHeaderSize,
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
        conn.consumeRecv(consumed);
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
        case opm::protocol::MessageType::LoginRequest:
            return handleLoginRequest(conn, payload);
        case opm::protocol::MessageType::UpdateProfileRequest:
            return handleUpdateProfileRequest(conn, payload);
        case opm::protocol::MessageType::LobbyListRequest:
            return handleLobbyListRequest(conn);
        case opm::protocol::MessageType::LobbyJoinRequest:
            return handleLobbyJoinRequest(conn, payload);
        case opm::protocol::MessageType::MovementInput:
            return handleMovementInput(conn, payload);
        case opm::protocol::MessageType::Ping:
            return handlePing(conn, payload);
        case opm::protocol::MessageType::LevelListRequest:
            return handleLevelListRequest(conn);
        case opm::protocol::MessageType::LevelLoadRequest:
            return handleLevelLoadRequest(conn, payload);
        case opm::protocol::MessageType::LevelSaveRequest:
            return handleLevelSaveRequest(conn, payload);
        case opm::protocol::MessageType::LobbySetLevelRequest:
            return handleLobbySetLevelRequest(conn, payload);
        case opm::protocol::MessageType::MapVoteRequest:
            return handleMapVoteRequest(conn, payload);
        case opm::protocol::MessageType::LeaveLobby:
            return handleLeaveLobby(conn);
        case opm::protocol::MessageType::CreateLobbyRequest:
            return handleCreateLobbyRequest(conn, payload);
        default:
            return true;
    }
}

bool Server::handleLobbyListRequest(ClientConnection& conn)
{
    const auto listing = lobbies_.listing();
    std::cout << "[server] LobbyListRequest from " << conn.session().username 
              << ": returning " << listing.size() << " lobbies\n";
    for (const auto& entry : listing) {
        std::cout << "  - " << entry.name << " (" << entry.players << "/" 
                  << entry.capacity << ")\n";
    }
    const auto payloadBytes = opm::protocol::encodeLobbyListPayload(listing);
    scratchWire_.build(opm::protocol::MessageType::LobbyListResponse, payloadBytes);
    return conn.send(scratchWire_.view());
}

bool Server::handleLobbyJoinRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto requestedLobby = opm::protocol::decodeLobbyJoinRequestPayload(scratchPayload_);

    // Save auth state before clearing session
    const auto savedAuthToken = conn.session().authToken;
    const auto savedUsername = conn.session().username;
    const auto savedDisplayName = conn.session().displayName;

    // Cleanly leave any existing lobby. Also release the simulation slot so
    // it stops participating in physics until the client lands in a new lobby.
    {
        const auto previousSlot = conn.session().playerIndex;
        if (previousSlot != kInvalidPlayerSlot) {
            simulation_.setPlayerActive(previousSlot, false);
            pendingInputs_[previousSlot] = {};
        }
    }
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
    conn.session() = PeerSession {
        .lobbyName = lobby->name,
        .playerIndex = *assignedSlot,
        .authToken = savedAuthToken,
        .username = savedUsername,
        .displayName = savedDisplayName
    };
    // Activate the simulation slot at the level spawn so it participates in
    // physics + player-vs-player collision.
    simulation_.respawnPlayer(*assignedSlot);

    // First-player-in-the-game: enter PreGame so the countdown starts
    // and clients see the "Get ready!" overlay. If a game was already
    // PreGame/Playing/GameOver this is a no-op — the new player joins
    // mid-state.
    bool wasEmpty = true;
    for (std::size_t i = 0; i < simulation_.state().players.size(); ++i) {
        if (i == *assignedSlot) {
            continue;
        }
        if (simulation_.state().players[i].active) {
            wasEmpty = false;
            break;
        }
    }
    if (wasEmpty) {
        enterPreGame();
    }

    if (!sendJoinResponse(conn, response)) {
        return false;
    }
    if (!sendLevelSnapshot(conn)) {
        return false;
    }
    broadcastRoster(*lobby);
    broadcastMapVoteUpdate();
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

bool Server::handleLevelListRequest(ClientConnection& conn)
{
    const auto names = levelStorage_.listNames();
    const auto payloadBytes = opm::protocol::encodeLevelListResponsePayload(names);
    scratchWire_.build(opm::protocol::MessageType::LevelListResponse, payloadBytes);
    std::cout << "[server] level list -> fd=" << conn.fd() << " count=" << names.size() << "\n";
    return conn.send(scratchWire_.view());
}

bool Server::handleLevelLoadRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto name = opm::protocol::decodeLevelLoadRequestPayload(scratchPayload_);

    opm::protocol::LevelLoadResponseData response;
    std::string err;
    if (levelStorage_.load(name, response.level, err)) {
        response.ok = true;
        response.reason = "loaded";
        std::cout << "[server] level loaded name=" << name
                  << " size=" << response.level.foliage.width << "x" << response.level.foliage.height << "\n";
    } else {
        response.ok = false;
        response.reason = err;
        std::cout << "[server] level load failed name=" << name << " reason=" << err << "\n";
    }
    const auto payloadBytes = opm::protocol::encodeLevelLoadResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::LevelLoadResponse, payloadBytes);
    return conn.send(scratchWire_.view());
}

bool Server::handleLevelSaveRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto request = opm::protocol::decodeLevelSaveRequestPayload(scratchPayload_);

    opm::protocol::LevelSaveResponseData response;
    std::string err;
    if (levelStorage_.save(request.name, request.level, err)) {
        response.ok = true;
        response.reason = "saved";
        std::cout << "[server] level saved name=" << request.name
                  << " size=" << request.level.foliage.width << "x" << request.level.foliage.height << "\n";
    } else {
        response.ok = false;
        response.reason = err;
        std::cout << "[server] level save failed name=" << request.name << " reason=" << err << "\n";
    }
    const auto payloadBytes = opm::protocol::encodeLevelSaveResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::LevelSaveResponse, payloadBytes);
    return conn.send(scratchWire_.view());
}

bool Server::handleLobbySetLevelRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto levelName = opm::protocol::decodeLobbySetLevelRequestPayload(scratchPayload_);

    opm::protocol::LobbySetLevelResponseData response;

    const auto& session = conn.session();
    if (session.playerIndex == kInvalidPlayerSlot) {
        response.ok = false;
        response.reason = "not_in_lobby";
        const auto bytes = opm::protocol::encodeLobbySetLevelResponsePayload(response);
        scratchWire_.build(opm::protocol::MessageType::LobbySetLevelResponse, bytes);
        return conn.send(scratchWire_.view());
    }

    opm::engine::LevelData level;
    std::string err;
    if (!levelStorage_.load(levelName, level, err)) {
        response.ok = false;
        response.reason = err;
        const auto bytes = opm::protocol::encodeLobbySetLevelResponsePayload(response);
        scratchWire_.build(opm::protocol::MessageType::LobbySetLevelResponse, bytes);
        return conn.send(scratchWire_.view());
    }

    simulation_.setLevel(level);
    std::cout << "[server] lobby '" << session.lobbyName << "' switched to level '" << levelName
              << "' (" << level.foliage.width << "x" << level.foliage.height << ")\n";

    response.ok = true;
    response.reason = "level_set";
    const auto bytes = opm::protocol::encodeLobbySetLevelResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::LobbySetLevelResponse, bytes);
    if (!conn.send(scratchWire_.view())) {
        return false;
    }

    Lobby* lobby = lobbies_.find(session.lobbyName);
    if (lobby != nullptr) {
        std::uint32_t playerCount = lobby->playerCount();
        std::cout << "[server] lobby '" << session.lobbyName << "' has " << playerCount << " connected player(s)\n";
        
        // Single-player mode: if only 1 player is in the lobby, immediately
        // transition to RoundStarting phase (skip voting/announce phases).
        // This keeps the server phase in sync with the client, which enters
        // Playing immediately after receiving the LevelSnapshot.
        if (playerCount == 1U) {
            std::cout << "[server] single-player mode detected, transitioning to RoundStarting\n";
            selectedMap_ = levelName;
            selectedTiebreak_ = false;
            enterRoundStarting();
            // enterRoundStarting() broadcasts the level snapshot, so don't broadcast again
        } else {
            // Multiplayer mode: broadcast level but stay in PreGame for voting
            broadcastLevelSnapshotToLobby(*lobby);
        }
    }
    return true;
}

void Server::broadcastLevelSnapshotToLobby(const Lobby& lobby)
{
    const auto payloadBytes = opm::protocol::encodeLevelSnapshotPayload(simulation_.level());
    WireBuilder wire;
    wire.build(opm::protocol::MessageType::LevelSnapshot, payloadBytes);
    for (const auto fd : lobby.slots) {
        if (fd != kInvalidSocket) {
            (void)sendAll(fd, wire.view());
        }
    }
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
    auto roster = lobby.roster();
    
    // Update each roster entry with authenticated displayName from connection if available
    for (auto& player : roster) {
        const auto fd = lobby.slots[player.playerIndex];
        if (fd != kInvalidSocket) {
            if (auto* conn = connections_.find(fd)) {
                const auto& session = conn->session();
                if (!session.displayName.empty()) {
                    player.displayName = session.displayName;
                }
            }
        }
    }
    
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
        if (auto* conn = connections_.find(fd); conn != nullptr) {
            const auto slot = conn->session().playerIndex;
            if (slot != kInvalidPlayerSlot) {
                simulation_.setPlayerActive(slot, false);
                pendingInputs_[slot] = {};
            }
        }
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
        // Only clear the one-shot "just pressed" flag. Held-state flags
        // (runHeld, moveLeft, moveRight, jumpHeld, crouchHeld) are
        // intentionally retained so a single delayed or missing packet does
        // not interrupt continuous hold states — P-speed requires runHeld to
        // be true on every tick without interruption.
        input.jumpPressed = false;
    }
    // During PreGame, players are confined to the spawn safe zone — if
    // physics moved someone outside (or they spawned just outside), pull
    // them back. Done after the step so the visible position next tick
    // is the clamped one.
    if (gamePhase_ == opm::protocol::GamePhase::PreGame
     || gamePhase_ == opm::protocol::GamePhase::RoundStarting) {
        confinePlayersToSafeZone();
    }
}

void Server::confinePlayersToSafeZone()
{
    const auto zone = opm::engine::computeSpawnSafeZone(simulation_.level());
    auto& players = simulation_.mutableState().players;
    // Pre-compute float bounds once — these are constant for all players.
    const float minX = static_cast<float>(zone.minX);
    const float maxX = static_cast<float>(zone.maxX) - opm::engine::kPlayerWidthTiles;
    const float minY = static_cast<float>(zone.minY);
    const float maxY = static_cast<float>(zone.maxY) - opm::engine::kPlayerHeightTiles;
    for (auto& p : players) {
        if (!p.active) {
            continue;
        }
        // Clamp the player AABB so its left edge >= zone.minX, right
        // edge <= zone.maxX (player width = kPlayerWidthTiles); same for
        // Y. If the clamp had to move us, also kill velocity in that axis
        // so we don't keep slamming into the invisible wall every tick.
        if (p.position.x < minX) { p.position.x = minX; if (p.velocity.x < 0) p.velocity.x = 0; }
        if (p.position.x > maxX) { p.position.x = maxX; if (p.velocity.x > 0) p.velocity.x = 0; }
        if (p.position.y < minY) { p.position.y = minY; if (p.velocity.y < 0) p.velocity.y = 0; }
        if (p.position.y > maxY) { p.position.y = maxY; if (p.velocity.y > 0) p.velocity.y = 0; }
    }
}

std::uint16_t Server::firstPlayerAtGoal() const
{
    const auto& level = simulation_.level();
    const float gx = level.goalX;
    const float gy = level.goalY;
    // Goal is a 1x1 tile; the player's AABB hitting it counts as a touch.
    const float gxRight = gx + 1.0F;
    const float gyTop   = gy + 1.0F;
    const auto& players = simulation_.state().players;
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto& p = players[i];
        if (!p.active) {
            continue;
        }
        const float pxRight = p.position.x + opm::engine::kPlayerWidthTiles;
        const float pyTop   = p.position.y + opm::engine::kPlayerHeightTiles;
        const bool overlaps =
            p.position.x < gxRight && pxRight > gx
         && p.position.y < gyTop   && pyTop   > gy;
        if (overlaps) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return kNoWinnerSlot;
}

void Server::enterPreGame()
{
    gamePhase_ = opm::protocol::GamePhase::PreGame;
    // Countdown is paused (0) until the first vote arrives. handleMapVoteRequest
    // bumps it to kVoteStartCountdownTicks on first vote and clamps to
    // kAllVotedCountdownTicks once every active player has voted.
    countdownTicks_ = 0;
    winnerSlot_ = kNoWinnerSlot;
    mapVotes_.clear();
    selectedMap_.clear();
    selectedTiebreak_ = false;
    // Refresh the available-levels list so the vote screen reflects what
    // the storage actually holds right now.
    availableLevels_ = levelStorage_.listNames();
    std::cout << "[server] phase -> PreGame (waiting for votes)\n";
}

void Server::enterRoundStarting()
{
    gamePhase_ = opm::protocol::GamePhase::RoundStarting;
    countdownTicks_ = kRoundStartCountdownTicks;
    winnerSlot_ = kNoWinnerSlot;
    // selectedMap_ was set by pickWinningMap() during the announce
    // sub-phase. Apply it here, then clear so the next PreGame starts
    // fresh.
    const std::string winningLevel = selectedMap_;
    selectedMap_.clear();
    selectedTiebreak_ = false;
    if (!winningLevel.empty()) {
        opm::engine::LevelData level;
        std::string err;
        if (levelStorage_.load(winningLevel, level, err)) {
            simulation_.setLevel(level);
            std::cout << "[server] phase -> RoundStarting on voted level '" << winningLevel << "'\n";
            const auto payloadBytes = opm::protocol::encodeLevelSnapshotPayload(simulation_.level());
            WireBuilder wire;
            wire.build(opm::protocol::MessageType::LevelSnapshot, payloadBytes);
            for (auto& [fd, conn] : connections_.map()) {
                (void)conn.send(wire.view());
            }
        } else {
            std::cout << "[server] phase -> RoundStarting (vote winner '" << winningLevel
                      << "' failed to load: " << err << "); keeping current level\n";
        }
    } else {
        std::cout << "[server] phase -> RoundStarting on current level (no votes)\n";
    }
    // Respawn all active players at the (possibly new) spawn.
    for (std::size_t i = 0; i < simulation_.state().players.size(); ++i) {
        if (simulation_.state().players[i].active) {
            simulation_.respawnPlayer(i);
        }
    }
}

void Server::enterPlaying()
{
    gamePhase_ = opm::protocol::GamePhase::Playing;
    countdownTicks_ = 0;
    winnerSlot_ = kNoWinnerSlot;
    std::cout << "[server] phase -> Playing (race begins)\n";
}

void Server::enterGameOver(std::uint16_t winnerSlot)
{
    gamePhase_ = opm::protocol::GamePhase::GameOver;
    countdownTicks_ = kGameOverDisplayTicks;
    winnerSlot_ = winnerSlot;
    std::cout << "[server] phase -> GameOver winner=" << winnerSlot << "\n";
}

void Server::pickWinningMap()
{
    selectedMap_.clear();
    selectedTiebreak_ = false;
    if (mapVotes_.empty()) {
        return;
    }
    // Tally non-empty votes per map.
    std::unordered_map<std::string, std::uint32_t> tally;
    for (const auto& [slot, name] : mapVotes_) {
        if (!name.empty()) {
            tally[name] += 1U;
        }
    }
    if (tally.empty()) {
        return;
    }
    // Find the highest count.
    std::uint32_t topCount = 0U;
    for (const auto& [name, count] : tally) {
        if (count > topCount) {
            topCount = count;
        }
    }
    // Collect every map at that count — these are the contenders.
    std::vector<std::string> contenders;
    contenders.reserve(tally.size());
    for (const auto& [name, count] : tally) {
        if (count == topCount) {
            contenders.push_back(name);
        }
    }
    if (contenders.size() == 1U) {
        selectedMap_ = contenders.front();
        selectedTiebreak_ = false;
    } else {
        std::uniform_int_distribution<std::size_t> dist(0U, contenders.size() - 1U);
        selectedMap_ = contenders[dist(rng_)];
        selectedTiebreak_ = true;
        std::cout << "[server] vote tied at " << topCount << " across "
                  << contenders.size() << " maps -> randomly picked '"
                  << selectedMap_ << "'\n";
    }
}

void Server::tickGamePhase()
{
    // Detect "no players left" and snap back to PreGame so the next
    // join starts clean.
    bool anyActive = false;
    for (const auto& p : simulation_.state().players) {
        if (p.active) {
            anyActive = true;
            break;
        }
    }
    if (!anyActive) {
        if (gamePhase_ != opm::protocol::GamePhase::PreGame || countdownTicks_ != 0U) {
            gamePhase_ = opm::protocol::GamePhase::PreGame;
            countdownTicks_ = 0U; // paused — counting resumes when a player joins
            winnerSlot_ = kNoWinnerSlot;
            mapVotes_.clear();
        }
        return;
    }

    switch (gamePhase_) {
        case opm::protocol::GamePhase::PreGame: {
            const bool announcing = !selectedMap_.empty();
            if (countdownTicks_ > 0U) {
                countdownTicks_ -= 1U;
                if (countdownTicks_ == 0U) {
                    if (announcing) {
                        // Announce sub-phase ended -> hand the round
                        // off to the gameplay screen, but with a 5s
                        // intro countdown during which players are
                        // confined to the spawn safezone.
                        enterRoundStarting();
                    } else {
                        // Voting sub-phase ended -> pick winning map and
                        // hold for kAnnounceTicks so clients can show
                        // the result before the gameplay screen takes
                        // over.
                        pickWinningMap();
                        countdownTicks_ = kAnnounceTicks;
                    }
                }
            }
            break;
        }
        case opm::protocol::GamePhase::RoundStarting:
            if (countdownTicks_ > 0U) {
                countdownTicks_ -= 1U;
                if (countdownTicks_ == 0U) {
                    enterPlaying();
                }
            }
            break;
        case opm::protocol::GamePhase::Playing: {
            const auto winner = firstPlayerAtGoal();
            if (winner != kNoWinnerSlot) {
                enterGameOver(winner);
            }
            break;
        }
        case opm::protocol::GamePhase::GameOver:
            if (countdownTicks_ > 0U) {
                countdownTicks_ -= 1U;
                if (countdownTicks_ == 0U) {
                    enterPreGame();
                }
            }
            break;
    }
}

void Server::tickBroadcastState()
{
    // Reuse persistent scratch vectors to avoid per-tick heap allocation.
    auto& data = scratchStateUpdate_;
    data.players.clear();
    data.actors.clear();

    data.serverTick = simulation_.state().tick;
    // Only broadcast active slots — the simulation has up to 500 slots and
    // sending every empty one each tick (~14 KB at 60 Hz) easily fills the
    // kernel send buffer of a freshly-joined client that's still loading
    // assets, causing a sendAll timeout and a forced disconnect. Sparse
    // encoding with an explicit slot index per record keeps this to ~30 B
    // per active player.
    const auto& players = simulation_.state().players;
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto& s = players[i];
        if (!s.active) {
            continue;
        }
        data.players.push_back(opm::protocol::PlayerNetState {
            .slotIndex = static_cast<std::uint16_t>(i),
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
            .style = static_cast<std::uint8_t>(s.style),
            .powerupTransitionFrames = s.powerupTransitionFrames,
            .invincibilityFrames = s.invincibilityFrames,
        });
    }
    data.actors.reserve(simulation_.state().actors.size());
    for (const auto& a : simulation_.state().actors) {
        data.actors.push_back(opm::protocol::ActorNetState {
            .positionX = a.position.x,
            .positionY = a.position.y,
            .velocityX = a.velocity.x,
            .velocityY = a.velocity.y,
            .alive = a.alive,
            .facingRight = a.facingRight,
            .script = static_cast<std::uint8_t>(a.script),
            .enemyKind = a.enemyKind,
            .category = static_cast<std::uint8_t>(a.category),
        });
    }
    data.phase = gamePhase_;
    data.countdownTicks = countdownTicks_;
    data.winnerSlot = winnerSlot_;
    data.selectedMap = selectedMap_;
    data.selectedTiebreak = selectedTiebreak_;
    // Encode into the persistent scratch buffer to avoid a per-tick heap allocation.
    opm::protocol::encodeStateUpdatePayload(data, scratchPayload_);
    broadcastWire_.build(opm::protocol::MessageType::StateUpdate, scratchPayload_);

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

bool Server::handleMapVoteRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    const auto& session = conn.session();
    if (session.playerIndex == kInvalidPlayerSlot) {
        return true; // not in lobby; ignore
    }
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto levelName = opm::protocol::decodeMapVoteRequestPayload(scratchPayload_);
    if (levelName.empty()) {
        mapVotes_.erase(session.playerIndex);
    } else {
        mapVotes_[session.playerIndex] = levelName;
    }

    // Drive the PreGame countdown off the vote state.
    if (gamePhase_ == opm::protocol::GamePhase::PreGame) {
        std::size_t activeCount = 0;
        for (const auto& p : simulation_.state().players) {
            if (p.active) {
                activeCount += 1;
            }
        }
        std::size_t nonEmptyVotes = 0;
        for (const auto& [s, n] : mapVotes_) {
            if (!n.empty()) {
                nonEmptyVotes += 1;
            }
        }
        // First vote of the round starts the timer at 60s.
        if (nonEmptyVotes >= 1U && countdownTicks_ == 0U) {
            countdownTicks_ = kVoteStartCountdownTicks;
            std::cout << "[server] vote received -> countdown started (" << kVoteStartCountdownTicks << " ticks)\n";
        }
        // Once everyone has voted, snap the remaining time down to 15s
        // (but never extend it).
        if (activeCount > 0U && nonEmptyVotes >= activeCount) {
            if (countdownTicks_ == 0U || countdownTicks_ > kAllVotedCountdownTicks) {
                countdownTicks_ = kAllVotedCountdownTicks;
                std::cout << "[server] all " << activeCount << " players voted -> countdown -> "
                          << kAllVotedCountdownTicks << " ticks\n";
            }
        }
    }

    broadcastMapVoteUpdate();
    return true;
}

void Server::broadcastMapVoteUpdate()
{
    std::vector<opm::protocol::MapVote> tally;
    tally.reserve(mapVotes_.size());
    for (const auto& [slot, name] : mapVotes_) {
        tally.push_back(opm::protocol::MapVote {.slotIndex = slot, .levelName = name});
    }
    const auto payloadBytes = opm::protocol::encodeMapVoteUpdatePayload(tally);
    WireBuilder wire;
    wire.build(opm::protocol::MessageType::MapVoteUpdate, payloadBytes);
    for (auto& [fd, conn] : connections_.map()) {
        if (conn.session().playerIndex == kInvalidPlayerSlot) {
            continue;
        }
        (void)conn.send(wire.view());
    }
}

bool Server::handleLoginRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    scratchPayload_.assign(payload.begin(), payload.end());
    const auto loginData = opm::protocol::decodeLoginRequestPayload(scratchPayload_);

    opm::protocol::LoginResponseData response;
    const auto authToken = accountManager_.login(loginData.username, loginData.password);

    if (authToken.has_value()) {
        response.ok = true;
        response.token = authToken->token;
        response.displayName = authToken->displayName;
        conn.session().authToken = authToken->token;
        conn.session().username = authToken->username;
        conn.session().displayName = authToken->displayName;
        std::cout << "[server] user '" << authToken->username << "' authenticated\n";
    } else {
        response.ok = false;
        response.reason = "invalid_credentials";
        std::cout << "[server] login failed for '" << loginData.username << "'\n";
    }

    const auto responsePayload = opm::protocol::encodeLoginResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::LoginResponse, responsePayload);
    return conn.send(scratchWire_.view());
}

bool Server::handleUpdateProfileRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    // Check if user is authenticated
    if (conn.session().authToken.empty()) {
        opm::protocol::UpdateProfileResponseData response;
        response.ok = false;
        response.reason = "not_authenticated";
        const auto responsePayload = opm::protocol::encodeUpdateProfileResponsePayload(response);
        scratchWire_.build(opm::protocol::MessageType::UpdateProfileResponse, responsePayload);
        return conn.send(scratchWire_.view());
    }

    scratchPayload_.assign(payload.begin(), payload.end());
    const auto profileData = opm::protocol::decodeUpdateProfileRequestPayload(scratchPayload_);

    opm::protocol::UpdateProfileResponseData response;
    if (accountManager_.updateDisplayName(conn.session().username, profileData.displayName)) {
        response.ok = true;
        conn.session().displayName = profileData.displayName;
        std::cout << "[server] user '" << conn.session().username << "' updated display name to '" 
                  << profileData.displayName << "'\n";
    } else {
        response.ok = false;
        response.reason = "update_failed";
    }

    const auto responsePayload = opm::protocol::encodeUpdateProfileResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::UpdateProfileResponse, responsePayload);
    return conn.send(scratchWire_.view());
}

bool Server::handleLeaveLobby(ClientConnection& conn)
{
    // Remove the player from any lobby they are in
    // This allows them to rejoin from the main menu without disconnecting
    lobbies_.removeFromAll(conn.fd());
    return true;
}

bool Server::handleCreateLobbyRequest(ClientConnection& conn, std::span<const std::uint8_t> payload)
{
    // Check if player is authenticated
    if (conn.session().authToken.empty() || conn.session().username.empty()) {
        opm::protocol::CreateLobbyResponseData response;
        response.ok = false;
        response.reason = "not_authenticated";
        const auto responsePayload = opm::protocol::encodeCreateLobbyResponsePayload(response);
        scratchWire_.build(opm::protocol::MessageType::CreateLobbyResponse, responsePayload);
        return conn.send(scratchWire_.view());
    }

    scratchPayload_.assign(payload.begin(), payload.end());
    const auto requestData = opm::protocol::decodeCreateLobbyRequestPayload(scratchPayload_);

    opm::protocol::CreateLobbyResponseData response;
    
    // Validate lobby name
    if (requestData.lobbyName.empty() || requestData.lobbyName.length() > 64) {
        response.ok = false;
        response.reason = "invalid_lobby_name";
    } else {
        // Try to create the lobby
        if (lobbies_.create(requestData.lobbyName)) {
            response.ok = true;
            std::cout << "[server] user '" << conn.session().username << "' created lobby '" 
                      << requestData.lobbyName << "'\n";
        } else {
            response.ok = false;
            response.reason = "lobby_already_exists";
        }
    }

    const auto responsePayload = opm::protocol::encodeCreateLobbyResponsePayload(response);
    scratchWire_.build(opm::protocol::MessageType::CreateLobbyResponse, responsePayload);
    return conn.send(scratchWire_.view());
}

} // namespace opm::server
