#pragma once

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
    RosterUpdate = 12,
    LevelListRequest = 13,
    LevelListResponse = 14,
    LevelLoadRequest = 15,
    LevelLoadResponse = 16,
    LevelSaveRequest = 17,
    LevelSaveResponse = 18,
    LobbySetLevelRequest = 19,
    LobbySetLevelResponse = 20
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
    std::uint16_t playerIndex {0xFFFFU};
    bool connected {false};
    std::uint8_t colorR {255U};
    std::uint8_t colorG {255U};
    std::uint8_t colorB {255U};
    std::string displayName {};
};

struct LobbyJoinResponseData {
    bool accepted {false};
    std::uint16_t playerIndex {0xFFFFU};
    std::uint32_t tickRateHz {60};
    std::string lobbyName {};
    std::string reason {};
    std::vector<PlayerInfo> roster {};
};

struct PlayerNetState {
    // Server-side simulation slot this state belongs to. Lets the server
    // send a sparse list (only active slots) instead of every slot every
    // tick — a 2-player session is ~60 B/tick instead of ~14 KB/tick.
    std::uint16_t slotIndex {0};
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
    std::uint8_t style {0}; // PlayerStyle (Small=0, Big=1)
    std::uint8_t powerupTransitionFrames {0};
    std::uint8_t invincibilityFrames {0};
};

struct ActorNetState {
    float positionX {0.0F};
    float positionY {0.0F};
    float velocityX {0.0F};
    float velocityY {0.0F};
    bool alive {true};
    bool facingRight {true};
    std::uint8_t script {0};
    std::uint8_t enemyKind {0};
    std::uint8_t category {0}; // ActorCategory (Enemy=0, Powerup=1)
};

struct StateUpdateData {
    std::uint32_t serverTick {0};
    std::vector<PlayerNetState> players {};
    std::vector<ActorNetState> actors {};
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

[[nodiscard]] std::vector<std::uint8_t> encodeLevelListResponsePayload(const std::vector<std::string>& names);
[[nodiscard]] std::vector<std::string> decodeLevelListResponsePayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeLevelLoadRequestPayload(const std::string& name);
[[nodiscard]] std::string decodeLevelLoadRequestPayload(const std::vector<std::uint8_t>& payload);

struct LevelLoadResponseData {
    bool ok {false};
    std::string reason {};
    opm::engine::LevelData level {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeLevelLoadResponsePayload(const LevelLoadResponseData& data);
[[nodiscard]] LevelLoadResponseData decodeLevelLoadResponsePayload(const std::vector<std::uint8_t>& payload);

struct LevelSaveRequestData {
    std::string name {};
    opm::engine::LevelData level {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeLevelSaveRequestPayload(const LevelSaveRequestData& data);
[[nodiscard]] LevelSaveRequestData decodeLevelSaveRequestPayload(const std::vector<std::uint8_t>& payload);

struct LevelSaveResponseData {
    bool ok {false};
    std::string reason {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeLevelSaveResponsePayload(const LevelSaveResponseData& data);
[[nodiscard]] LevelSaveResponseData decodeLevelSaveResponsePayload(const std::vector<std::uint8_t>& payload);

[[nodiscard]] std::vector<std::uint8_t> encodeLobbySetLevelRequestPayload(const std::string& levelName);
[[nodiscard]] std::string decodeLobbySetLevelRequestPayload(const std::vector<std::uint8_t>& payload);

struct LobbySetLevelResponseData {
    bool ok {false};
    std::string reason {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeLobbySetLevelResponsePayload(const LobbySetLevelResponseData& data);
[[nodiscard]] LobbySetLevelResponseData decodeLobbySetLevelResponsePayload(const std::vector<std::uint8_t>& payload);

} // namespace opm::protocol
