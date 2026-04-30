#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "opm/engine.hpp"
#include "opm/level.hpp"

namespace opm::protocol {

enum class MessageType : std::uint8_t {
    Hello = 1,
    LobbyListRequest = 2,
    LobbyListResponse = 3,
    MovementInput = 4,
    StateHash = 5,
    Ping = 6,
    Pong = 7,
    LobbyJoinRequest = 8,
    LobbyJoinResponse = 9,
    LevelSnapshot = 10,
    StateUpdate = 11,
    RosterUpdate = 12
};

struct Message {
    MessageType type {MessageType::Hello};
    std::vector<std::uint8_t> payload {};
};

struct LobbyEntry {
    std::string name {};
    std::uint32_t players {0};
    std::uint32_t capacity {0};
};

struct PlayerInfo {
    std::uint8_t playerIndex {0xFFU};
    bool connected {false};
    std::uint8_t colorR {255U};
    std::uint8_t colorG {255U};
    std::uint8_t colorB {255U};
    std::string displayName {};
};

struct LobbyJoinResponseData {
    bool accepted {false};
    std::uint8_t playerIndex {0xFFU};
    std::uint32_t tickRateHz {60};
    std::string lobbyName {};
    std::string reason {};
    std::vector<PlayerInfo> roster {};
};

struct PlayerNetState {
    float positionX {0.0F};
    float positionY {0.0F};
    float velocityX {0.0F};
    float velocityY {0.0F};
    bool onGround {true};
    bool facingRight {true};
    bool skidding {false};
    bool crouching {false};
    bool pSpeedActive {false};
    float pSpeedMeter {0.0F};
};

struct StateUpdateData {
    std::uint32_t serverTick {0};
    std::array<PlayerNetState, 2> players {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeMessage(const Message& message);
[[nodiscard]] Message decodeMessage(const std::vector<std::uint8_t>& bytes);

[[nodiscard]] std::vector<std::uint8_t> payloadFromString(const std::string& text);
[[nodiscard]] std::string payloadToString(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeLobbyListPayload(const std::vector<LobbyEntry>& lobbies);
[[nodiscard]] std::vector<LobbyEntry> decodeLobbyListPayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeLobbyJoinRequestPayload(const std::string& lobbyName);
[[nodiscard]] std::string decodeLobbyJoinRequestPayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeLobbyJoinResponsePayload(const LobbyJoinResponseData& payload);
[[nodiscard]] LobbyJoinResponseData decodeLobbyJoinResponsePayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeLevelSnapshotPayload(const opm::engine::LevelData& level);
[[nodiscard]] opm::engine::LevelData decodeLevelSnapshotPayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeMovementInputPayload(const opm::engine::InputFrame& input);
[[nodiscard]] opm::engine::InputFrame decodeMovementInputPayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeStateUpdatePayload(const StateUpdateData& update);
[[nodiscard]] StateUpdateData decodeStateUpdatePayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeRosterUpdatePayload(const std::vector<PlayerInfo>& roster);
[[nodiscard]] std::vector<PlayerInfo> decodeRosterUpdatePayload(const std::vector<std::uint8_t>& payload);

} // namespace opm::protocol
