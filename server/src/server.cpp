#include "server.hpp"

#include "net/sender.hpp"
#include "runtime/scoped_timer_resolution.hpp"
#include "runtime/scoped_wsa.hpp"
#include "runtime/tick_pacer.hpp"

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
    conn.session() = PeerSession {.lobbyName = lobby->name, .playerIndex = *assignedSlot};
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
        broadcastLevelSnapshotToLobby(*lobby);
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
        input = {};
    }
    // During PreGame, players are confined to the spawn safe zone — if
    // physics moved someone outside (or they spawned just outside), pull
    // them back. Done after the step so the visible position next tick
    // is the clamped one.
    if (gamePhase_ == opm::protocol::GamePhase::PreGame) {
        confinePlayersToSafeZone();
    }
}

void Server::confinePlayersToSafeZone()
{
    const auto zone = opm::engine::computeSpawnSafeZone(simulation_.level());
    auto& players = simulation_.mutableState().players;
    for (auto& p : players) {
        if (!p.active) {
            continue;
        }
        // Clamp the player AABB so its left edge >= zone.minX, right
        // edge <= zone.maxX (player width = kPlayerWidthTiles); same for
        // Y. If the clamp had to move us, also kill velocity in that axis
        // so we don't keep slamming into the invisible wall every tick.
        const float minX = static_cast<float>(zone.minX);
        const float maxX = static_cast<float>(zone.maxX) - opm::engine::kPlayerWidthTiles;
        const float minY = static_cast<float>(zone.minY);
        const float maxY = static_cast<float>(zone.maxY) - opm::engine::kPlayerHeightTiles;
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

void Server::enterPlaying()
{
    gamePhase_ = opm::protocol::GamePhase::Playing;
    countdownTicks_ = 0;
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
            std::cout << "[server] phase -> Playing on voted level '" << winningLevel << "'\n";
            const auto payloadBytes = opm::protocol::encodeLevelSnapshotPayload(simulation_.level());
            WireBuilder wire;
            wire.build(opm::protocol::MessageType::LevelSnapshot, payloadBytes);
            for (auto& [fd, conn] : connections_.map()) {
                (void)conn.send(wire.view());
            }
        } else {
            std::cout << "[server] phase -> Playing (vote winner '" << winningLevel
                      << "' failed to load: " << err << "); keeping current level\n";
        }
    } else {
        std::cout << "[server] phase -> Playing on current level (no votes)\n";
    }
    // Respawn all active players at the (possibly new) spawn.
    for (std::size_t i = 0; i < simulation_.state().players.size(); ++i) {
        if (simulation_.state().players[i].active) {
            simulation_.respawnPlayer(i);
        }
    }
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
                        // Announce sub-phase ended -> start the round.
                        enterPlaying();
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
    // Encode StateUpdate to wire bytes ONCE per tick, then broadcast raw.
    opm::protocol::StateUpdateData data;
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

} // namespace opm::server
