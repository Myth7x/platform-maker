#include "server_app.hpp"

#include "opm/engine.hpp"
#include "opm/protocol.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace opm::server {
namespace {

volatile std::sig_atomic_t gKeepRunning = 1;

constexpr std::uint32_t kTickRateHz = 60U;
constexpr std::size_t kProtocolHeaderSize = 9U;

struct LobbyRuntime {
    std::string name {};
    std::uint32_t capacity {2};
    std::array<int, 2> slots {-1, -1};
};

struct PeerSession {
    std::string lobbyName {};
    std::uint8_t playerIndex {0xFFU};
};

std::array<opm::protocol::PlayerInfo, 2> buildLobbyRoster(const LobbyRuntime& lobby)
{
    std::array<opm::protocol::PlayerInfo, 2> roster {};
    const std::array<std::string, 2> names {"Player 1", "Player 2"};
    const std::array<std::array<std::uint8_t, 3>, 2> colors {
        std::array<std::uint8_t, 3> {224U, 60U, 60U},
        std::array<std::uint8_t, 3> {70U, 190U, 80U},
    };

    for (std::size_t i = 0; i < roster.size(); ++i) {
        roster[i].playerIndex = static_cast<std::uint8_t>(i);
        roster[i].connected = lobby.slots[i] >= 0;
        roster[i].colorR = colors[i][0];
        roster[i].colorG = colors[i][1];
        roster[i].colorB = colors[i][2];
        roster[i].displayName = names[i];
    }

    return roster;
}

void handleSignal(int)
{
    gKeepRunning = 0;
}

bool setNonBlocking(const int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool sendAll(const int fd, const std::uint8_t* data, const std::size_t size)
{
    std::size_t sent = 0U;
    while (sent < size) {
        const ssize_t bytes = send(fd, data + sent, size - sent, 0);
        if (bytes <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(bytes);
    }
    return true;
}

bool sendMessage(const int fd, const opm::protocol::Message& message)
{
    const auto bytes = opm::protocol::encodeMessage(message);
    return sendAll(fd, bytes.data(), bytes.size());
}

bool extractMessages(std::vector<std::uint8_t>& buffer, std::vector<std::vector<std::uint8_t>>& packets)
{
    packets.clear();

    while (buffer.size() >= kProtocolHeaderSize) {
        const std::uint32_t payloadSize = readU32(buffer, 5U);
        const std::size_t packetSize = kProtocolHeaderSize + static_cast<std::size_t>(payloadSize);
        if (buffer.size() < packetSize) {
            break;
        }

        packets.emplace_back(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(packetSize));
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(packetSize));
    }

    return true;
}

std::vector<opm::protocol::LobbyEntry> buildLobbyList(const std::vector<LobbyRuntime>& lobbies)
{
    std::vector<opm::protocol::LobbyEntry> entries;
    entries.reserve(lobbies.size());

    for (const auto& lobby : lobbies) {
        std::uint32_t playerCount = 0;
        for (const int fd : lobby.slots) {
            if (fd >= 0) {
                playerCount += 1U;
            }
        }

        entries.push_back(opm::protocol::LobbyEntry {
            .name = lobby.name,
            .players = playerCount,
            .capacity = lobby.capacity,
        });
    }

    return entries;
}

std::optional<std::size_t> findLobbyByName(const std::vector<LobbyRuntime>& lobbies, const std::string& name)
{
    for (std::size_t i = 0; i < lobbies.size(); ++i) {
        if (lobbies[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

void removeClientFromLobbies(std::vector<LobbyRuntime>& lobbies, const int fd)
{
    for (auto& lobby : lobbies) {
        for (auto& slot : lobby.slots) {
            if (slot == fd) {
                slot = -1;
            }
        }
    }
}

void dropClient(
    const int fd,
    std::vector<LobbyRuntime>& lobbies,
    std::unordered_map<int, PeerSession>& sessions,
    std::unordered_map<int, std::vector<std::uint8_t>>& recvBuffers
)
{
    removeClientFromLobbies(lobbies, fd);
    sessions.erase(fd);
    recvBuffers.erase(fd);
    close(fd);
}

opm::protocol::StateUpdateData buildStateUpdate(const opm::engine::Simulation& simulation)
{
    opm::protocol::StateUpdateData update;
    update.serverTick = simulation.state().tick;

    for (std::size_t i = 0; i < update.players.size(); ++i) {
        const auto& state = simulation.state().players[i];
        update.players[i] = opm::protocol::PlayerNetState {
            .positionX = state.position.x,
            .positionY = state.position.y,
            .velocityX = state.velocity.x,
            .velocityY = state.velocity.y,
            .onGround = state.onGround,
            .facingRight = state.facingRight,
            .skidding = state.skidding,
            .crouching = state.crouching,
            .pSpeedActive = state.pSpeedActive,
            .pSpeedMeter = state.pSpeedMeter,
        };
    }

    return update;
}

} // namespace

ServerApp::ServerApp(const std::uint16_t port)
    : port_(port)
{
}

int ServerApp::run()
{
    std::signal(SIGINT, handleSignal);

    const int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::cerr << "[server] failed to create socket\n";
        return 1;
    }

    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "[server] failed to bind port " << port_ << "\n";
        close(listenFd);
        return 1;
    }

    if (listen(listenFd, 64) != 0) {
        std::cerr << "[server] failed to listen on port " << port_ << "\n";
        close(listenFd);
        return 1;
    }

    if (!setNonBlocking(listenFd)) {
        std::cerr << "[server] failed to set listen socket non-blocking\n";
        close(listenFd);
        return 1;
    }

    std::cout << "[server] custom TCP protocol online at port " << port_ << "\n";

    opm::engine::Simulation simulation;
    std::vector<LobbyRuntime> lobbies {
        LobbyRuntime {.name = "default_lobby", .capacity = 2U},
        LobbyRuntime {.name = "race_lobby", .capacity = 2U},
    };

    std::unordered_map<int, PeerSession> sessions;
    std::unordered_map<int, std::vector<std::uint8_t>> recvBuffers;
    std::array<opm::engine::InputFrame, 2> pendingInputs {};

    constexpr auto tickDuration = std::chrono::milliseconds(16);
    constexpr std::size_t kRecvChunkSize = 4096U;

    while (gKeepRunning != 0) {
        auto tickStart = std::chrono::steady_clock::now();

        while (true) {
            sockaddr_in clientAddr {};
            socklen_t clientLen = sizeof(clientAddr);
            const int clientFd = accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientFd < 0) {
                break;
            }

            if (!setNonBlocking(clientFd)) {
                close(clientFd);
                continue;
            }

            recvBuffers[clientFd] = {};
            std::cout << "[server] client connected fd=" << clientFd << "\n";
        }

        std::vector<int> toDrop;

        for (auto& [fd, buffer] : recvBuffers) {
            bool shouldDrop = false;
            std::array<std::uint8_t, kRecvChunkSize> recvChunk {};

            while (true) {
                const ssize_t bytesRead = recv(fd, recvChunk.data(), recvChunk.size(), MSG_DONTWAIT);
                if (bytesRead == 0) {
                    shouldDrop = true;
                    break;
                }
                if (bytesRead < 0) {
                    break;
                }

                buffer.insert(buffer.end(), recvChunk.begin(), recvChunk.begin() + bytesRead);
            }

            if (shouldDrop) {
                toDrop.push_back(fd);
                continue;
            }

            std::vector<std::vector<std::uint8_t>> packets;
            extractMessages(buffer, packets);

            for (const auto& packet : packets) {
                try {
                    const auto message = opm::protocol::decodeMessage(packet);
                    if (message.type == opm::protocol::MessageType::LobbyListRequest) {
                        const bool ok = sendMessage(
                            fd,
                            opm::protocol::Message {
                                .type = opm::protocol::MessageType::LobbyListResponse,
                                .payload = opm::protocol::encodeLobbyListPayload(buildLobbyList(lobbies)),
                            });
                        if (!ok) {
                            toDrop.push_back(fd);
                            break;
                        }
                    } else if (message.type == opm::protocol::MessageType::LobbyJoinRequest) {
                        const auto requestedLobby = opm::protocol::decodeLobbyJoinRequestPayload(message.payload);

                        removeClientFromLobbies(lobbies, fd);
                        sessions.erase(fd);

                        opm::protocol::LobbyJoinResponseData joinResponse;
                        joinResponse.tickRateHz = kTickRateHz;
                        joinResponse.lobbyName = requestedLobby;

                        const auto lobbyIndex = findLobbyByName(lobbies, requestedLobby);
                        if (!lobbyIndex.has_value()) {
                            joinResponse.accepted = false;
                            joinResponse.reason = "lobby_not_found";
                            if (!sendMessage(fd, opm::protocol::Message {
                                    .type = opm::protocol::MessageType::LobbyJoinResponse,
                                    .payload = opm::protocol::encodeLobbyJoinResponsePayload(joinResponse),
                                })) {
                                toDrop.push_back(fd);
                                break;
                            }
                        } else {
                            auto& lobby = lobbies[*lobbyIndex];
                            std::uint8_t assignedSlot = 0xFFU;
                            for (std::size_t slotIndex = 0; slotIndex < lobby.slots.size(); ++slotIndex) {
                                if (lobby.slots[slotIndex] < 0) {
                                    lobby.slots[slotIndex] = fd;
                                    assignedSlot = static_cast<std::uint8_t>(slotIndex);
                                    break;
                                }
                            }

                            if (assignedSlot == 0xFFU) {
                                joinResponse.accepted = false;
                                joinResponse.reason = "lobby_full";
                                joinResponse.roster.clear();
                                if (!sendMessage(fd, opm::protocol::Message {
                                        .type = opm::protocol::MessageType::LobbyJoinResponse,
                                        .payload = opm::protocol::encodeLobbyJoinResponsePayload(joinResponse),
                                    })) {
                                    toDrop.push_back(fd);
                                    break;
                                }
                            } else {
                                joinResponse.accepted = true;
                                joinResponse.playerIndex = assignedSlot;
                                joinResponse.reason = "joined";
                                sessions[fd] = PeerSession {.lobbyName = lobby.name, .playerIndex = assignedSlot};

                                const auto roster = buildLobbyRoster(lobby);
                                joinResponse.roster.assign(roster.begin(), roster.end());

                                if (!sendMessage(fd, opm::protocol::Message {
                                        .type = opm::protocol::MessageType::LobbyJoinResponse,
                                        .payload = opm::protocol::encodeLobbyJoinResponsePayload(joinResponse),
                                    })) {
                                    toDrop.push_back(fd);
                                    break;
                                }

                                if (!sendMessage(fd, opm::protocol::Message {
                                        .type = opm::protocol::MessageType::LevelSnapshot,
                                        .payload = opm::protocol::encodeLevelSnapshotPayload(simulation.level()),
                                    })) {
                                    toDrop.push_back(fd);
                                    break;
                                }
                            }
                        }
                    } else if (message.type == opm::protocol::MessageType::MovementInput) {
                        const auto sessionIt = sessions.find(fd);
                        if (sessionIt != sessions.end() && sessionIt->second.playerIndex < pendingInputs.size()) {
                            pendingInputs[sessionIt->second.playerIndex] = opm::protocol::decodeMovementInputPayload(message.payload);
                        }
                    }
                } catch (const std::exception& ex) {
                    std::cerr << "[server] dropped malformed packet from fd=" << fd << " reason=" << ex.what() << "\n";
                }
            }
        }

        for (const int fd : toDrop) {
            std::cout << "[server] client disconnected fd=" << fd << "\n";
            dropClient(fd, lobbies, sessions, recvBuffers);
        }

        for (auto& input : pendingInputs) {
            input.frameIndex = simulation.state().tick;
        }
        simulation.step(pendingInputs);
        for (auto& input : pendingInputs) {
            input = {};
        }

        const auto updatePayload = opm::protocol::encodeStateUpdatePayload(buildStateUpdate(simulation));
        std::vector<int> failedSends;
        for (const auto& [fd, session] : sessions) {
            (void)session;
            const bool ok = sendMessage(
                fd,
                opm::protocol::Message {
                    .type = opm::protocol::MessageType::StateUpdate,
                    .payload = updatePayload,
                });
            if (!ok) {
                failedSends.push_back(fd);
            }
        }

        for (const int fd : failedSends) {
            std::cout << "[server] client send failure fd=" << fd << "\n";
            dropClient(fd, lobbies, sessions, recvBuffers);
        }

        if (simulation.state().tick % 300U == 0U) {
            std::cout << "[server] tick=" << simulation.state().tick << " hash=" << simulation.stateHash() << "\n";
        }

        std::this_thread::sleep_until(tickStart + tickDuration);
    }

    for (const auto& [fd, _] : recvBuffers) {
        (void)_;
        close(fd);
    }
    close(listenFd);

    std::cout << "[server] shutdown\n";
    return 0;
}

} // namespace opm::server
