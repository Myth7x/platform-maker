#include "net_client.hpp"

#include "net/socket_compat.hpp"
#include "opm/protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace opm::client::net {
namespace {

constexpr std::size_t kProtocolHeaderSize = 9U;
constexpr std::size_t kRecvChunkSize = 4096U;

#ifdef _WIN32
struct WsaInitGuard {
    WsaInitGuard()
    {
        WSADATA wsaData;
        ok_ = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    }
    ~WsaInitGuard()
    {
        if (ok_) {
            WSACleanup();
        }
    }
    bool ok_ {false};
};
WsaInitGuard& wsaGuard()
{
    static WsaInitGuard guard;
    return guard;
}
#endif

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool setNonBlocking(const socket_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool waitForFd(const socket_t fd, const short events, const std::uint32_t timeoutMs)
{
    pollfd_t pfd {};
    pfd.fd = fd;
    pfd.events = events;

#ifdef _WIN32
    const int result = sockPoll(&pfd, 1, static_cast<INT>(timeoutMs));
#else
    const int result = sockPoll(&pfd, 1, static_cast<int>(timeoutMs));
#endif
    if (result <= 0) {
        return false;
    }

    return (pfd.revents & events) != 0;
}

bool sendAll(const socket_t fd, const std::uint8_t* data, const std::size_t size, std::string& status)
{
    std::size_t sent = 0U;
    while (sent < size) {
        const ssize_t bytes = ::send(fd, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (bytes > 0) {
            sent += static_cast<std::size_t>(bytes);
            continue;
        }

        if (sockWouldBlock()) {
            if (!waitForFd(fd, POLLOUT, 250U)) {
                status = "socket_send_timeout";
                return false;
            }
            continue;
        }

        status = "socket_send_failed";
        return false;
    }

    status = "ok";
    return true;
}

bool sendMessage(const socket_t fd, const opm::protocol::Message& message, std::string& status)
{
    const auto bytes = opm::protocol::encodeMessage(message);
    return sendAll(fd, bytes.data(), bytes.size(), status);
}

bool tryExtractMessage(std::vector<std::uint8_t>& buffer, opm::protocol::Message& message)
{
    if (buffer.size() < kProtocolHeaderSize) {
        return false;
    }

    const std::uint32_t payloadSize = readU32(buffer, 5U);
    const std::size_t packetSize = kProtocolHeaderSize + static_cast<std::size_t>(payloadSize);
    if (buffer.size() < packetSize) {
        return false;
    }

    std::vector<std::uint8_t> packet(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(packetSize));
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(packetSize));

    message = opm::protocol::decodeMessage(packet);
    return true;
}

LevelSnapshot levelDataToSnapshot(const opm::engine::LevelData& level)
{
    LevelSnapshot snap;
    snap.width = level.foliage.width;
    snap.height = level.foliage.height;
    snap.spawnX = level.spawnX;
    snap.spawnY = level.spawnY;
    snap.goalX = level.goalX;
    snap.goalY = level.goalY;
    snap.background = level.background.tileIndices;
    snap.foliage = level.foliage.tileIndices;
    snap.foreground = level.foreground.tileIndices;
    return snap;
}

// Drain all currently-extractable messages, routing side-channel ones
// (Roster / Pong, plus optionally LevelSnapshot) through the router.
// Returns the first message the router didn't consume (which the
// caller then handles), or false if the buffer ran out before such a
// message was found. The optional `expected` parameter sets whether
// LevelSnapshot is treated as a side-channel.
//
// Free helper rather than a member because both pollStateUpdate and
// awaitMessage want the same behavior with slightly different "what
// counts as side-channel" semantics.
bool drainKnownMessages(std::vector<std::uint8_t>& recvBuffer,
                        MessageRouter& router,
                        opm::protocol::Message& outForCaller,
                        bool treatLevelSnapshotAsSideChannel)
{
    while (tryExtractMessage(recvBuffer, outForCaller)) {
        if (!router.dispatch(outForCaller, treatLevelSnapshotAsSideChannel)) {
            return true;
        }
    }
    return false;
}

} // namespace

SessionClient::SessionClient()
{
#ifdef _WIN32
    (void)wsaGuard();
#endif
}

SessionClient::~SessionClient()
{
    disconnect();
}

bool SessionClient::pumpRecv(const std::uint32_t timeoutMs, std::string& status)
{
    if (!waitForFd(fd_, POLLIN, timeoutMs)) {
        status = "socket_receive_timeout";
        return false;
    }

    std::array<std::uint8_t, kRecvChunkSize> chunk {};
    const ssize_t bytesRead = ::recv(fd_, reinterpret_cast<char*>(chunk.data()),
        static_cast<int>(chunk.size()), 0);
    if (bytesRead <= 0) {
        status = "socket_receive_failed";
        return false;
    }
    recvBuffer_.insert(recvBuffer_.end(), chunk.begin(), chunk.begin() + bytesRead);
    return true;
}

bool SessionClient::connect(const std::string& host, const std::uint16_t port, const std::uint32_t timeoutMs, std::string& status)
{
    disconnect();

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
        status = "socket_resolve_failed";
        return false;
    }

    socket_t fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd == kInvalidSocket) {
        freeaddrinfo(result);
        status = "socket_create_failed";
        return false;
    }

    if (!setNonBlocking(fd)) {
        freeaddrinfo(result);
        closesocketCompat(fd);
        status = "socket_nonblocking_failed";
        return false;
    }

    // Disable Nagle's algorithm for accurate ping and snappy input.
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    const int connectResult = ::connect(fd, result->ai_addr, static_cast<int>(result->ai_addrlen));
    if (connectResult != 0) {
        if (!sockInProgress()) {
            freeaddrinfo(result);
            closesocketCompat(fd);
            status = "socket_connect_failed";
            return false;
        }

        if (!waitForFd(fd, POLLOUT, timeoutMs)) {
            freeaddrinfo(result);
            closesocketCompat(fd);
            status = "socket_connect_timeout";
            return false;
        }

        int error = 0;
        socklen_t len = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) != 0 || error != 0) {
            freeaddrinfo(result);
            closesocketCompat(fd);
            status = "socket_connect_failed";
            return false;
        }
    }

    freeaddrinfo(result);

    fd_ = fd;
    recvBuffer_.clear();
    status = "ok";
    return true;
}

bool SessionClient::requestLobbyList(const std::uint32_t timeoutMs, std::vector<LobbyInfo>& lobbies, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    if (!sendMessage(fd_, opm::protocol::Message {.type = opm::protocol::MessageType::LobbyListRequest, .payload = {}}, status)) {
        return false;
    }

    opm::protocol::Message response;
    while (!tryExtractMessage(recvBuffer_, response)) {
        if (!pumpRecv(timeoutMs, status)) {
            return false;
        }
    }

    if (response.type != opm::protocol::MessageType::LobbyListResponse) {
        status = "unexpected_message_type";
        return false;
    }

    const auto decoded = opm::protocol::decodeLobbyListPayload(response.payload);
    lobbies.clear();
    lobbies.reserve(decoded.size());
    for (const auto& entry : decoded) {
        lobbies.push_back(LobbyInfo {.name = entry.name, .players = entry.players, .capacity = entry.capacity});
    }

    status = "ok";
    return true;
}

bool SessionClient::joinLobby(const std::string& lobbyName, const std::uint32_t timeoutMs, JoinResult& result, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    if (!sendMessage(
            fd_,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::LobbyJoinRequest,
                .payload = opm::protocol::encodeLobbyJoinRequestPayload(lobbyName),
            },
            status)) {
        return false;
    }

    opm::protocol::Message response;
    while (!tryExtractMessage(recvBuffer_, response)) {
        if (!pumpRecv(timeoutMs, status)) {
            return false;
        }
    }

    if (response.type != opm::protocol::MessageType::LobbyJoinResponse) {
        status = "unexpected_message_type";
        return false;
    }

    const auto payload = opm::protocol::decodeLobbyJoinResponsePayload(response.payload);
    result = JoinResult {
        .accepted = payload.accepted,
        .playerIndex = payload.playerIndex,
        .tickRateHz = payload.tickRateHz,
        .lobbyName = payload.lobbyName,
        .reason = payload.reason,
        .roster = payload.roster,
    };

    status = payload.reason.empty() ? "ok" : payload.reason;
    return payload.accepted;
}

bool SessionClient::receiveLevelSnapshot(const std::uint32_t timeoutMs, LevelSnapshot& snapshot, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    opm::protocol::Message message;
    while (!drainKnownMessages(recvBuffer_, router_, message, /*treatLevelSnapshotAsSideChannel=*/false)) {
        if (!pumpRecv(timeoutMs, status)) {
            return false;
        }
    }

    if (message.type != opm::protocol::MessageType::LevelSnapshot) {
        status = "unexpected_message_type";
        return false;
    }

    snapshot = levelDataToSnapshot(opm::protocol::decodeLevelSnapshotPayload(message.payload));
    status = "ok";
    return true;
}

bool SessionClient::requestLogin(const std::string& username, const std::string& password,
    const std::uint32_t timeoutMs, opm::protocol::LoginResponseData& response, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    const opm::protocol::LoginRequestData request {.username = username, .password = password};
    if (!sendMessage(fd_,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::LoginRequest,
                .payload = opm::protocol::encodeLoginRequestPayload(request),
            },
            status)) {
        return false;
    }

    opm::protocol::Message wireResponse;
    if (!awaitMessage(opm::protocol::MessageType::LoginResponse, timeoutMs, wireResponse, status)) {
        return false;
    }

    response = opm::protocol::decodeLoginResponsePayload(wireResponse.payload);
    if (!response.ok) {
        status = response.reason.empty() ? "invalid_credentials" : response.reason;
        return false;
    }

    status = "ok";
    return true;
}

bool SessionClient::requestUpdateProfile(const std::string& displayName,
    const std::uint32_t timeoutMs, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    const opm::protocol::UpdateProfileRequestData request {.displayName = displayName};
    if (!sendMessage(fd_,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::UpdateProfileRequest,
                .payload = opm::protocol::encodeUpdateProfileRequestPayload(request),
            },
            status)) {
        return false;
    }

    opm::protocol::Message response;
    if (!awaitMessage(opm::protocol::MessageType::UpdateProfileResponse, timeoutMs, response, status)) {
        return false;
    }

    const auto decoded = opm::protocol::decodeUpdateProfileResponsePayload(response.payload);
    if (!decoded.ok) {
        status = decoded.reason.empty() ? "profile_update_failed" : decoded.reason;
        return false;
    }

    status = "ok";
    return true;
}

bool SessionClient::sendMovementInput(const opm::engine::InputFrame& input, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    return sendMessage(
        fd_,
        opm::protocol::Message {
            .type = opm::protocol::MessageType::MovementInput,
            .payload = opm::protocol::encodeMovementInputPayload(input),
        },
        status);
}

bool SessionClient::sendMapVote(const std::string& levelName, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }
    return sendMessage(
        fd_,
        opm::protocol::Message {
            .type = opm::protocol::MessageType::MapVoteRequest,
            .payload = opm::protocol::encodeMapVoteRequestPayload(levelName),
        },
        status);
}

void SessionClient::sendPingIfDue(const std::uint32_t intervalMs)
{
    if (fd_ == kInvalidSocket) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastPingSentAt_.time_since_epoch().count() != 0) {
        const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPingSentAt_).count();
        if (sinceLast < static_cast<std::int64_t>(intervalMs)) {
            return;
        }
    }
    lastPingSentAt_ = now;

    const auto nowNs = static_cast<std::uint64_t>(now.time_since_epoch().count());
    std::vector<std::uint8_t> payload(sizeof(nowNs));
    std::memcpy(payload.data(), &nowNs, sizeof(nowNs));

    std::string ignoredStatus;
    (void)sendMessage(
        fd_,
        opm::protocol::Message {
            .type = opm::protocol::MessageType::Ping,
            .payload = std::move(payload),
        },
        ignoredStatus);
}

bool SessionClient::pollStateUpdate(const std::uint32_t timeoutMs, StateUpdate& update, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    opm::protocol::Message message;
    while (!drainKnownMessages(recvBuffer_, router_, message, /*treatLevelSnapshotAsSideChannel=*/true)) {
        if (!pumpRecv(timeoutMs, status)) {
            return false;
        }
    }

    if (message.type != opm::protocol::MessageType::StateUpdate) {
        status = "unexpected_message_type";
        return false;
    }

    const auto payload = opm::protocol::decodeStateUpdatePayload(message.payload);
    update.serverTick = payload.serverTick;
    update.phase = payload.phase;
    update.countdownTicks = payload.countdownTicks;
    update.winnerSlot = payload.winnerSlot;
    update.selectedMap = payload.selectedMap;
    update.selectedTiebreak = payload.selectedTiebreak;
    update.players.clear();
    update.players.reserve(payload.players.size());
    for (const auto& player : payload.players) {
        update.players.push_back(RemotePlayerState {
            .slotIndex = player.slotIndex,
            .positionX = player.positionX,
            .positionY = player.positionY,
            .velocityX = player.velocityX,
            .velocityY = player.velocityY,
            .onGround = player.onGround,
            .facingRight = player.facingRight,
            .skidding = player.skidding,
            .crouching = player.crouching,
            .pSpeedActive = player.pSpeedActive,
            .pSpeedMeter = player.pSpeedMeter,
            .style = player.style,
            .powerupTransitionFrames = player.powerupTransitionFrames,
            .invincibilityFrames = player.invincibilityFrames,
        });
    }
    update.actors.clear();
    update.actors.reserve(payload.actors.size());
    for (const auto& a : payload.actors) {
        update.actors.push_back(RemoteActorState {
            .positionX = a.positionX,
            .positionY = a.positionY,
            .velocityX = a.velocityX,
            .velocityY = a.velocityY,
            .alive = a.alive,
            .facingRight = a.facingRight,
            .script = a.script,
            .enemyKind = a.enemyKind,
            .category = a.category,
        });
    }

    status = "ok";
    return true;
}

void SessionClient::drainRosterUpdates(std::vector<std::vector<opm::protocol::PlayerInfo>>& out)
{
    router_.drainRosterUpdates(out);
}

void SessionClient::drainLevelSnapshots(std::vector<LevelSnapshot>& out)
{
    router_.drainLevelSnapshots(out);
}

bool SessionClient::requestSetLobbyLevel(const std::string& levelName,
    std::uint32_t timeoutMs, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }
    if (!sendMessage(fd_,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::LobbySetLevelRequest,
                .payload = opm::protocol::encodeLobbySetLevelRequestPayload(levelName),
            },
            status)) {
        return false;
    }
    opm::protocol::Message response;
    if (!awaitMessage(opm::protocol::MessageType::LobbySetLevelResponse, timeoutMs, response, status)) {
        return false;
    }
    const auto data = opm::protocol::decodeLobbySetLevelResponsePayload(response.payload);
    if (!data.ok) {
        status = data.reason.empty() ? "lobby_set_level_refused" : data.reason;
        return false;
    }
    status = "ok";
    return true;
}

void SessionClient::disconnect()
{
    if (fd_ != kInvalidSocket) {
        closesocketCompat(fd_);
        fd_ = kInvalidSocket;
    }

    recvBuffer_.clear();
    router_.reset();
    lastPingSentAt_ = {};
}

std::uint32_t SessionClient::getPingMs() const
{
    if (fd_ == kInvalidSocket) {
        return 0;
    }
    return router_.pingMs();
}

bool SessionClient::isConnected() const
{
    return fd_ != kInvalidSocket;
}

bool SessionClient::awaitMessage(opm::protocol::MessageType expected,
    std::uint32_t timeoutMs,
    opm::protocol::Message& out,
    std::string& status)
{
    const bool snapshotIsSideChannel = (expected != opm::protocol::MessageType::LevelSnapshot);
    while (true) {
        while (!drainKnownMessages(recvBuffer_, router_, out, snapshotIsSideChannel)) {
            if (!pumpRecv(timeoutMs, status)) {
                return false;
            }
        }
        // Once the player has joined a lobby the server broadcasts
        // StateUpdate every tick. Those would otherwise interrupt
        // request-reply flows (e.g. requestLevelList). Discard them
        // here — next pollStateUpdate picks up the latest.
        if (out.type == opm::protocol::MessageType::StateUpdate) {
            continue;
        }
        if (out.type == expected) {
            return true;
        }
        status = "unexpected_message_type";
        return false;
    }
}

bool SessionClient::requestLevelList(std::uint32_t timeoutMs, std::vector<std::string>& names, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }
    if (!sendMessage(fd_,
            opm::protocol::Message {.type = opm::protocol::MessageType::LevelListRequest, .payload = {}},
            status)) {
        return false;
    }
    opm::protocol::Message response;
    if (!awaitMessage(opm::protocol::MessageType::LevelListResponse, timeoutMs, response, status)) {
        return false;
    }
    names = opm::protocol::decodeLevelListResponsePayload(response.payload);
    status = "ok";
    return true;
}

bool SessionClient::requestLoadLevel(const std::string& name, std::uint32_t timeoutMs,
    opm::engine::LevelData& level, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }
    if (!sendMessage(fd_,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::LevelLoadRequest,
                .payload = opm::protocol::encodeLevelLoadRequestPayload(name),
            },
            status)) {
        return false;
    }
    opm::protocol::Message response;
    if (!awaitMessage(opm::protocol::MessageType::LevelLoadResponse, timeoutMs, response, status)) {
        return false;
    }
    const auto data = opm::protocol::decodeLevelLoadResponsePayload(response.payload);
    if (!data.ok) {
        status = data.reason.empty() ? "level_load_refused" : data.reason;
        return false;
    }
    level = data.level;
    status = "ok";
    return true;
}

bool SessionClient::requestSaveLevel(const std::string& name, const opm::engine::LevelData& level,
    std::uint32_t timeoutMs, std::string& status)
{
    if (fd_ == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }
    opm::protocol::LevelSaveRequestData request {.name = name, .level = level};
    if (!sendMessage(fd_,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::LevelSaveRequest,
                .payload = opm::protocol::encodeLevelSaveRequestPayload(request),
            },
            status)) {
        return false;
    }
    opm::protocol::Message response;
    if (!awaitMessage(opm::protocol::MessageType::LevelSaveResponse, timeoutMs, response, status)) {
        return false;
    }
    const auto data = opm::protocol::decodeLevelSaveResponsePayload(response.payload);
    if (!data.ok) {
        status = data.reason.empty() ? "level_save_refused" : data.reason;
        return false;
    }
    status = "ok";
    return true;
}

std::vector<LobbyInfo> requestLobbyList(
    const std::string& host,
    const std::uint16_t port,
    const std::uint32_t timeoutMs,
    std::string& status
)
{
    SessionClient client;
    if (!client.connect(host, port, timeoutMs, status)) {
        return {};
    }

    std::vector<LobbyInfo> lobbies;
    if (!client.requestLobbyList(timeoutMs, lobbies, status)) {
        return {};
    }

    return lobbies;
}

bool joinLobby(
    const std::string& host,
    const std::uint16_t port,
    const std::string& lobbyName,
    const std::uint32_t timeoutMs,
    JoinResult* joinResult,
    std::string& status
)
{
    SessionClient client;
    if (!client.connect(host, port, timeoutMs, status)) {
        return false;
    }

    JoinResult localResult;
    const bool accepted = client.joinLobby(lobbyName, timeoutMs, localResult, status);
    if (joinResult != nullptr) {
        *joinResult = localResult;
    }

    return accepted;
}

} // namespace opm::client::net
