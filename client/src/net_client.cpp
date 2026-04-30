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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace opm::client::net {
namespace {

constexpr std::size_t kProtocolHeaderSize = 9U;

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool setNonBlocking(const int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool waitForFd(const int fd, const short events, const std::uint32_t timeoutMs)
{
    pollfd pfd {};
    pfd.fd = fd;
    pfd.events = events;

    const int result = poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (result <= 0) {
        return false;
    }

    return (pfd.revents & events) != 0;
}

bool sendAll(const int fd, const std::uint8_t* data, const std::size_t size, std::string& status)
{
    std::size_t sent = 0U;
    while (sent < size) {
        const ssize_t bytes = send(fd, data + sent, size - sent, 0);
        if (bytes > 0) {
            sent += static_cast<std::size_t>(bytes);
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

bool sendMessage(const int fd, const opm::protocol::Message& message, std::string& status)
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
    int fd {-1};
    std::vector<std::uint8_t> recvBuffer {};
    std::uint32_t pingMs {0};
    std::chrono::system_clock::time_point lastMessageTime {};
    std::uint32_t lastPingSendTick {0};
    std::uint32_t currentTick {0};
};

SessionClient::SessionClient()
    : impl_(new Impl {})
{
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

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        status = "socket_create_failed";
        return false;
    }

    if (!setNonBlocking(fd)) {
        freeaddrinfo(result);
        close(fd);
        status = "socket_nonblocking_failed";
        return false;
    }

    const int connectResult = ::connect(fd, result->ai_addr, result->ai_addrlen);
    if (connectResult != 0) {
        if (errno != EINPROGRESS) {
            freeaddrinfo(result);
            close(fd);
            status = "socket_connect_failed";
            return false;
        }

        if (!waitForFd(fd, POLLOUT, timeoutMs)) {
            freeaddrinfo(result);
            close(fd);
            status = "socket_connect_timeout";
            return false;
        }

        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
            freeaddrinfo(result);
            close(fd);
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
    if (impl_->fd < 0) {
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
        const ssize_t bytesRead = recv(impl_->fd, chunk.data(), chunk.size(), 0);
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
    if (impl_->fd < 0) {
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
        const ssize_t bytesRead = recv(impl_->fd, chunk.data(), chunk.size(), 0);
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
    if (impl_->fd < 0) {
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
        const ssize_t bytesRead = recv(impl_->fd, chunk.data(), chunk.size(), 0);
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
    if (impl_->fd < 0) {
        status = "socket_not_connected";
        return false;
    }

    impl_->lastMessageTime = std::chrono::system_clock::now();
    return sendMessage(
        impl_->fd,
        opm::protocol::Message {
            .type = opm::protocol::MessageType::MovementInput,
            .payload = opm::protocol::encodeMovementInputPayload(input),
        },
        status);
}

bool SessionClient::pollStateUpdate(const std::uint32_t timeoutMs, StateUpdate& update, std::string& status)
{
    if (impl_->fd < 0) {
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
        const ssize_t bytesRead = recv(impl_->fd, chunk.data(), chunk.size(), 0);
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

    // Calculate ping based on round-trip time
    const auto now = std::chrono::system_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->lastMessageTime);
    if (elapsed.count() > 0) {
        impl_->pingMs = static_cast<std::uint32_t>(elapsed.count());
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

void SessionClient::disconnect()
{
    if (impl_ == nullptr) {
        return;
    }

    if (impl_->fd >= 0) {
        close(impl_->fd);
        impl_->fd = -1;
    }

    impl_->recvBuffer.clear();
    impl_->pingMs = 0;
    impl_->lastPingSendTick = 0;
    impl_->currentTick = 0;
}

std::uint32_t SessionClient::getPingMs() const
{
    if (impl_ == nullptr || impl_->fd < 0) {
        return 0;
    }
    return impl_->pingMs;
}

bool SessionClient::isConnected() const
{
    if (impl_ == nullptr) {
        return false;
    }
    return impl_->fd >= 0;
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
