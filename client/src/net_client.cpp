#include "net_client.hpp"

#include "opm/protocol.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
using ssize_t = long long;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static inline int closesocketCompat(socket_t s) { return ::closesocket(s); }
static inline bool sockWouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
static inline bool sockInProgress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
static inline int sockPoll(WSAPOLLFD* fds, ULONG nfds, INT timeoutMs) { return ::WSAPoll(fds, nfds, timeoutMs); }
using pollfd_t = WSAPOLLFD;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static inline int closesocketCompat(socket_t s) { return ::close(s); }
static inline bool sockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
static inline bool sockInProgress() { return errno == EINPROGRESS; }
static inline int sockPoll(pollfd* fds, nfds_t nfds, int timeoutMs) { return ::poll(fds, nfds, timeoutMs); }
using pollfd_t = pollfd;
#endif

namespace opm::client::net {
namespace {

constexpr std::size_t kProtocolHeaderSize = 9U;

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
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
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

} // namespace

struct SessionClient::Impl {
    socket_t fd {kInvalidSocket};
    std::vector<std::uint8_t> recvBuffer {};
    std::uint32_t pingMs {0};
    std::chrono::steady_clock::time_point lastPingSentAt {};
    bool hasPingSample {false};
    std::vector<std::vector<opm::protocol::PlayerInfo>> pendingRosters {};
};

SessionClient::SessionClient()
    : impl_(new Impl {})
{
#ifdef _WIN32
    (void)wsaGuard();
#endif
}

SessionClient::~SessionClient()
{
    disconnect();
    delete impl_;
    impl_ = nullptr;
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

    impl_->fd = fd;
    impl_->recvBuffer.clear();
    status = "ok";
    return true;
}

bool SessionClient::requestLobbyList(const std::uint32_t timeoutMs, std::vector<LobbyInfo>& lobbies, std::string& status)
{
    if (impl_->fd == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    if (!sendMessage(impl_->fd, opm::protocol::Message {.type = opm::protocol::MessageType::LobbyListRequest, .payload = {}}, status)) {
        return false;
    }

    opm::protocol::Message response;
    while (true) {
        if (tryExtractMessage(impl_->recvBuffer, response)) {
            break;
        }

        if (!waitForFd(impl_->fd, POLLIN, timeoutMs)) {
            status = "socket_receive_timeout";
            return false;
        }

        std::array<std::uint8_t, 4096> chunk {};
        const ssize_t bytesRead = ::recv(impl_->fd, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
        if (bytesRead <= 0) {
            status = "socket_receive_failed";
            return false;
        }
        impl_->recvBuffer.insert(impl_->recvBuffer.end(), chunk.begin(), chunk.begin() + bytesRead);
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
    if (impl_->fd == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    if (!sendMessage(
            impl_->fd,
            opm::protocol::Message {
                .type = opm::protocol::MessageType::LobbyJoinRequest,
                .payload = opm::protocol::encodeLobbyJoinRequestPayload(lobbyName),
            },
            status)) {
        return false;
    }

    opm::protocol::Message response;
    while (true) {
        if (tryExtractMessage(impl_->recvBuffer, response)) {
            break;
        }

        if (!waitForFd(impl_->fd, POLLIN, timeoutMs)) {
            status = "socket_receive_timeout";
            return false;
        }

        std::array<std::uint8_t, 4096> chunk {};
        const ssize_t bytesRead = ::recv(impl_->fd, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
        if (bytesRead <= 0) {
            status = "socket_receive_failed";
            return false;
        }
        impl_->recvBuffer.insert(impl_->recvBuffer.end(), chunk.begin(), chunk.begin() + bytesRead);
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
    if (impl_->fd == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    opm::protocol::Message message;
    while (true) {
        if (tryExtractMessage(impl_->recvBuffer, message)) {
            break;
        }

        if (!waitForFd(impl_->fd, POLLIN, timeoutMs)) {
            status = "socket_receive_timeout";
            return false;
        }

        std::array<std::uint8_t, 4096> chunk {};
        const ssize_t bytesRead = ::recv(impl_->fd, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
        if (bytesRead <= 0) {
            status = "socket_receive_failed";
            return false;
        }
        impl_->recvBuffer.insert(impl_->recvBuffer.end(), chunk.begin(), chunk.begin() + bytesRead);
    }

    if (message.type != opm::protocol::MessageType::LevelSnapshot) {
        status = "unexpected_message_type";
        return false;
    }

    const auto level = opm::protocol::decodeLevelSnapshotPayload(message.payload);
    snapshot.width = level.groundLayer.width;
    snapshot.height = level.groundLayer.height;
    snapshot.spawnX = level.spawnX;
    snapshot.spawnY = level.spawnY;
    snapshot.goalX = level.goalX;
    snapshot.goalY = level.goalY;
    snapshot.tiles = level.groundLayer.tileIndices;

    status = "ok";
    return true;
}

bool SessionClient::sendMovementInput(const opm::engine::InputFrame& input, std::string& status)
{
    if (impl_->fd == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    return sendMessage(
        impl_->fd,
        opm::protocol::Message {
            .type = opm::protocol::MessageType::MovementInput,
            .payload = opm::protocol::encodeMovementInputPayload(input),
        },
        status);
}

void SessionClient::sendPingIfDue(const std::uint32_t intervalMs)
{
    if (impl_ == nullptr || impl_->fd == kInvalidSocket) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (impl_->lastPingSentAt.time_since_epoch().count() != 0) {
        const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->lastPingSentAt).count();
        if (sinceLast < static_cast<std::int64_t>(intervalMs)) {
            return;
        }
    }
    impl_->lastPingSentAt = now;

    const auto nowNs = static_cast<std::uint64_t>(now.time_since_epoch().count());
    std::vector<std::uint8_t> payload(sizeof(nowNs));
    std::memcpy(payload.data(), &nowNs, sizeof(nowNs));

    std::string ignoredStatus;
    (void)sendMessage(
        impl_->fd,
        opm::protocol::Message {
            .type = opm::protocol::MessageType::Ping,
            .payload = std::move(payload),
        },
        ignoredStatus);
}

bool SessionClient::pollStateUpdate(const std::uint32_t timeoutMs, StateUpdate& update, std::string& status)
{
    if (impl_->fd == kInvalidSocket) {
        status = "socket_not_connected";
        return false;
    }

    opm::protocol::Message message;
    while (true) {
        if (tryExtractMessage(impl_->recvBuffer, message)) {
            if (message.type == opm::protocol::MessageType::RosterUpdate) {
                impl_->pendingRosters.push_back(opm::protocol::decodeRosterUpdatePayload(message.payload));
                continue;
            }
            if (message.type == opm::protocol::MessageType::Pong) {
                if (message.payload.size() == sizeof(std::uint64_t)) {
                    std::uint64_t echoedNs = 0;
                    std::memcpy(&echoedNs, message.payload.data(), sizeof(echoedNs));
                    const auto sentAt = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(echoedNs));
                    const auto rtt = std::chrono::steady_clock::now() - sentAt;
                    const auto rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(rtt).count();
                    if (rttMs >= 0 && rttMs < 60000) {
                        if (!impl_->hasPingSample) {
                            impl_->pingMs = static_cast<std::uint32_t>(rttMs);
                            impl_->hasPingSample = true;
                        } else {
                            // Exponential moving average to smooth jitter.
                            impl_->pingMs = static_cast<std::uint32_t>(
                                (impl_->pingMs * 3 + static_cast<std::uint32_t>(rttMs)) / 4);
                        }
                    }
                }
                continue;
            }
            break;
        }

        if (!waitForFd(impl_->fd, POLLIN, timeoutMs)) {
            status = "socket_receive_timeout";
            return false;
        }

        std::array<std::uint8_t, 4096> chunk {};
        const ssize_t bytesRead = ::recv(impl_->fd, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
        if (bytesRead <= 0) {
            status = "socket_receive_failed";
            return false;
        }
        impl_->recvBuffer.insert(impl_->recvBuffer.end(), chunk.begin(), chunk.begin() + bytesRead);
    }

    if (message.type != opm::protocol::MessageType::StateUpdate) {
        status = "unexpected_message_type";
        return false;
    }

    const auto payload = opm::protocol::decodeStateUpdatePayload(message.payload);
    update.serverTick = payload.serverTick;
    for (std::size_t i = 0; i < update.players.size(); ++i) {
        update.players[i] = RemotePlayerState {
            .positionX = payload.players[i].positionX,
            .positionY = payload.players[i].positionY,
            .velocityX = payload.players[i].velocityX,
            .velocityY = payload.players[i].velocityY,
            .onGround = payload.players[i].onGround,
            .facingRight = payload.players[i].facingRight,
            .skidding = payload.players[i].skidding,
            .crouching = payload.players[i].crouching,
            .pSpeedActive = payload.players[i].pSpeedActive,
            .pSpeedMeter = payload.players[i].pSpeedMeter,
        };
    }

    status = "ok";
    return true;
}

void SessionClient::drainRosterUpdates(std::vector<std::vector<opm::protocol::PlayerInfo>>& out)
{
    if (impl_ == nullptr) {
        return;
    }
    out.insert(out.end(),
        std::make_move_iterator(impl_->pendingRosters.begin()),
        std::make_move_iterator(impl_->pendingRosters.end()));
    impl_->pendingRosters.clear();
}

void SessionClient::disconnect()
{
    if (impl_ == nullptr) {
        return;
    }

    if (impl_->fd != kInvalidSocket) {
        closesocketCompat(impl_->fd);
        impl_->fd = kInvalidSocket;
    }

    impl_->recvBuffer.clear();
    impl_->pendingRosters.clear();
    impl_->pingMs = 0;
    impl_->hasPingSample = false;
    impl_->lastPingSentAt = {};
}

std::uint32_t SessionClient::getPingMs() const
{
    if (impl_ == nullptr || impl_->fd == kInvalidSocket) {
        return 0;
    }
    return impl_->pingMs;
}

bool SessionClient::isConnected() const
{
    if (impl_ == nullptr) {
        return false;
    }
    return impl_->fd != kInvalidSocket;
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
