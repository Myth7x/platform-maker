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
    LobbySetLevelResponse = 20,
    MapVoteRequest = 21,    // client -> server: "I vote for level X"
    MapVoteUpdate = 22,     // server -> all in lobby: tally of votes
    LoginRequest = 23,      // client -> server: username + password
    LoginResponse = 24,     // server -> client: auth result + token + display name
    UpdateProfileRequest = 25,   // client -> server: update display name
    UpdateProfileResponse = 26,  // server -> client: profile update result
    LeaveLobby = 27,        // client -> server: leave current lobby (stay connected)
    CreateLobbyRequest = 28,     // client -> server: create new lobby with name
    CreateLobbyResponse = 29,    // server -> client: create result
};

// Server-tracked game phase for the active session. Broadcast on every
// StateUpdate so clients can render the right HUD overlay.
enum class GamePhase : std::uint8_t {
    PreGame = 0,        // lobby: voting + announce; players confined to safezone
    Playing = 1,        // race in progress, first-to-goal wins
    GameOver = 2,       // someone won (or everyone left); brief display before reset
    RoundStarting = 3,  // post-lobby intro: 5s countdown, players confined to safezone
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
    bool isDying {false};
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

    // Game phase + countdown + winner. Sent every tick so clients always
    // know whether to render the lobby/countdown/winner overlay.
    GamePhase phase {GamePhase::PreGame};
    std::uint32_t countdownTicks {0};   // remaining ticks in current PreGame / GameOver phase
    std::uint16_t winnerSlot {0xFFFFU}; // valid only when phase == GameOver

    // Announce sub-phase (still GamePhase::PreGame): the map the
    // server picked when voting ended. Empty during the voting
    // sub-phase. `selectedTiebreak` is true when the pick was a
    // random tiebreak across multiple top-voted maps.
    std::string selectedMap {};
    bool        selectedTiebreak {false};
};

// One vote, sent from a client to the server. The server tallies and
// broadcasts a MapVoteUpdate when the tally changes.
struct MapVote {
    std::uint16_t slotIndex {0xFFFFU}; // which player voted (server fills this in for outgoing tallies)
    std::string levelName {};          // the level they voted for (empty = withdraw vote)
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
// Buffer-reuse overload: clears `out` and fills it in-place, avoiding a
// per-call heap allocation. Use this in the 60 Hz broadcast hot path.
void encodeStateUpdatePayload(const StateUpdateData& update, std::vector<std::uint8_t>& out);
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

// Map vote: client request payload is just the level name (empty = withdraw).
[[nodiscard]] std::vector<std::uint8_t> encodeMapVoteRequestPayload(const std::string& levelName);
[[nodiscard]] std::string decodeMapVoteRequestPayload(const std::vector<std::uint8_t>& payload);

// Map vote tally broadcast: list of (slotIndex, levelName) pairs.
[[nodiscard]] std::vector<std::uint8_t> encodeMapVoteUpdatePayload(const std::vector<MapVote>& votes);
[[nodiscard]] std::vector<MapVote> decodeMapVoteUpdatePayload(const std::vector<std::uint8_t>& payload);

struct LobbySetLevelResponseData {
    bool ok {false};
    std::string reason {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeLobbySetLevelResponsePayload(const LobbySetLevelResponseData& data);
[[nodiscard]] LobbySetLevelResponseData decodeLobbySetLevelResponsePayload(const std::vector<std::uint8_t>& payload);

// ---- Authentication ----

struct LoginRequestData {
    std::string username {};
    std::string password {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeLoginRequestPayload(const LoginRequestData& data);
[[nodiscard]] LoginRequestData decodeLoginRequestPayload(const std::vector<std::uint8_t>& payload);

struct LoginResponseData {
    bool ok {false};
    std::string reason {};
    std::string token {};           // session token if successful
    std::string displayName {};     // player's display name
};

[[nodiscard]] std::vector<std::uint8_t> encodeLoginResponsePayload(const LoginResponseData& data);
[[nodiscard]] LoginResponseData decodeLoginResponsePayload(const std::vector<std::uint8_t>& payload);

struct UpdateProfileRequestData {
    std::string displayName {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeUpdateProfileRequestPayload(const UpdateProfileRequestData& data);
[[nodiscard]] UpdateProfileRequestData decodeUpdateProfileRequestPayload(const std::vector<std::uint8_t>& payload);

struct UpdateProfileResponseData {
    bool ok {false};
    std::string reason {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeUpdateProfileResponsePayload(const UpdateProfileResponseData& data);
[[nodiscard]] UpdateProfileResponseData decodeUpdateProfileResponsePayload(const std::vector<std::uint8_t>& payload);

// Leave lobby: no payload needed
[[nodiscard]] inline std::vector<std::uint8_t> encodeLeaveLobbyPayload() { return {}; }
inline void decodeLeaveLobbyPayload(const std::vector<std::uint8_t>&) {}

// Create lobby
struct CreateLobbyRequestData {
    std::string lobbyName {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeCreateLobbyRequestPayload(const CreateLobbyRequestData& data);
[[nodiscard]] CreateLobbyRequestData decodeCreateLobbyRequestPayload(const std::vector<std::uint8_t>& payload);

struct CreateLobbyResponseData {
    bool ok {false};
    std::string reason {};
};

[[nodiscard]] std::vector<std::uint8_t> encodeCreateLobbyResponsePayload(const CreateLobbyResponseData& data);
[[nodiscard]] CreateLobbyResponseData decodeCreateLobbyResponsePayload(const std::vector<std::uint8_t>& payload);

} // namespace opm::protocol
