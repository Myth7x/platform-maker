#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "opm/level.hpp"

namespace opm::engine {

// SMB1 small Mario approximates about 12x16 pixels in a 16px tile grid.
inline constexpr float kPlayerWidthTiles = 0.75F;
inline constexpr float kPlayerHeightTiles = 1.0F;

// Player-vs-player AABB used for pushing and head-stomp landing. This is
// wider/taller than the tile-collision body so that contact happens when the
// rendered sprite pixels visually touch, not when the tighter tile-collision
// boxes would touch. The interaction box is centered horizontally on the
// tile-collision body and aligned at the bottom (feet).
inline constexpr float kPlayerInteractionWidthTiles = 1.0F;
inline constexpr float kPlayerInteractionHeightTiles = 1.0F;

struct Vec2 {
    float x {0.0F};
    float y {0.0F};
};

// Visual / power-up state of a player. Drives which sprite the client
// renders and (eventually) which body size collisions use. For now only
// affects rendering and the "extra hit" interaction with enemies.
enum class PlayerStyle : std::uint8_t {
    Small = 0,
    Big = 1,
};

struct InputFrame {
    std::uint32_t frameIndex {0};
    bool moveLeft {false};
    bool moveRight {false};
    bool jumpPressed {false};
    bool jumpHeld {false};
    bool runHeld {false};
    bool crouchHeld {false};
};

struct PlayerState {
    Vec2 position {};
    Vec2 velocity {};
    bool onGround {true};
    bool facingRight {true};
    bool skidding {false};
    bool crouching {false};
    bool pSpeedActive {false};
    // Whether this slot represents a real, playing participant. Inactive
    // slots are not integrated and are ignored by player-vs-player collision.
    bool active {false};
    // Visual / power-up tier. Mushrooms upgrade Small -> Big; an enemy hit
    // downgrades Big -> Small (instead of respawning).
    PlayerStyle style {PlayerStyle::Small};
    // Frames remaining in the power-up transition. While > 0 the engine
    // freezes player input, zeroes velocity, and treats them as invincible
    // to enemy contact. The renderer flashes between the current and the
    // opposite style for the duration so the upgrade reads visually.
    std::uint8_t powerupTransitionFrames {0};
    // Post-damage invincibility window. Engine grants this when a Big
    // player gets hit and downgrades to Small — the player keeps moving
    // (no freeze) but enemy contact is ignored, and the renderer blinks
    // the sprite on/off so the i-frames read visually.
    std::uint8_t invincibilityFrames {0};
    std::uint8_t jumpHoldFrames {0};
    std::uint8_t jumpBufferFrames {0};
    std::uint8_t coyoteFrames {0};
    float pSpeedMeter {0.0F};
};

// Runtime state of an enemy/NPC actor. Spawned from a LevelData::actors
// placement when a level is loaded; integrated each tick by the simulation.
struct ActorState {
    Vec2 position {};
    Vec2 velocity {};
    ActorScript script {ActorScript::MoveRandom};
    bool alive {true};
    bool facingRight {true};
    bool onGround {false};
    // Behavior toggles copied from ActorSpawn at level load:
    //   diesWhenStomped  — landing on the actor's head kills it instead of
    //                      damaging the player (player gets a small bounce).
    //   canJumpObstacles — when the actor walks into a wall while grounded,
    //                      it jumps to try to clear it.
    //   canJumpRandom    — periodically jumps on a deterministic schedule.
    //   canFly           — gravity is disabled; with MoveToPlayer the actor
    //                      also chases the player vertically.
    bool diesWhenStomped {false};
    bool canJumpObstacles {false};
    bool canJumpRandom {false};
    bool canFly {false};
    // Visual sprite kind (mirrors ActorSpawn::enemyKind). Pure render data.
    std::uint8_t enemyKind {0};
    // Top-level category — Enemy or Powerup. Drives the player-overlap
    // outcome (damage vs style upgrade).
    ActorCategory category {ActorCategory::Enemy};
    // Per-script counter (e.g. frames until next random direction change).
    std::uint16_t scriptTimer {0};
    // Cooldown between random jumps so a flapping actor doesn't spam.
    std::uint16_t jumpCooldown {0};
    // Frames of "held" jump gravity remaining. While > 0 and vy > 0 the
    // actor falls slower, mirroring the player's space-hold mechanic.
    // The obstacle-jump AI sets this to the minimum hold needed to clear
    // the obstacle in front of the actor.
    std::uint8_t jumpHoldFrames {0};
};

// Damage event raised when an actor touches a player. Consumed each tick by
// the server (to broadcast / respond to it).
struct ActorDamageEvent {
    std::uint16_t actorIndex {0};
    std::uint16_t playerIndex {0};
};

struct GameState {
    std::uint32_t tick {0};
    std::vector<PlayerState> players {};
    std::vector<ActorState> actors {};
    // Filled by step() each tick. Cleared at the start of the next step().
    std::vector<ActorDamageEvent> damageEvents {};
};

class Simulation {
public:
    Simulation();

    void setPlayerCount(std::size_t count);
    void reset();
    void setLevel(LevelData level);
    void step(std::span<const InputFrame> inputs);

    // Toggle whether a player slot participates in simulation. Inactive
    // slots are not integrated each tick and are ignored by player-vs-player
    // collision (so unused server-side slots don't pile up at spawn).
    void setPlayerActive(std::size_t index, bool active);

    // Reset a player slot back to the level's spawn position with cleared
    // velocity / state, and mark it active. Use when a player joins or
    // (re)spawns mid-game.
    void respawnPlayer(std::size_t index);

    [[nodiscard]] const GameState& state() const;
    [[nodiscard]] const LevelData& level() const;
    [[nodiscard]] std::uint64_t stateHash() const;

    // Write access to game state — used by the server for post-step
    // constraints (e.g. clamping players to a safe zone during a
    // pre-game countdown). Don't mutate during step().
    [[nodiscard]] GameState& mutableState();

private:
    void integratePlayer(PlayerState& player, const InputFrame& input,
        const std::vector<PlayerState>& others, std::size_t selfIndex);

    // Resolves horizontal AABB overlaps between active players by mutually
    // displacing them (the "pushing" behavior). Each candidate move is
    // clamped against solid tiles. Called once per tick after integration.
    void resolvePlayerPushes();

    // Steps every alive actor: gravity + script-driven horizontal motion +
    // tile-collision sweep + damage detection against active players.
    void stepActors();

    // Returns the X reachable starting from `fromX` and aiming for `targetX`
    // at vertical position `y`, clamped by solid tiles. Used by the push
    // resolver. Assumes the requested travel is at most ~1 tile (true for
    // sub-frame push corrections).
    [[nodiscard]] float sweepHorizontalAgainstTiles(float fromX, float targetX, float y) const;

    GameState state_ {};
    LevelData level_ {};
    // Reused scratch for the per-tick player position snapshot taken at the
    // start of step() so that integratePlayer reads a stable state regardless
    // of integration order. Promoted from a local variable to avoid the
    // per-tick heap allocation (one vector<PlayerState> copy per frame).
    std::vector<PlayerState> playerSnapshot_ {};
    // Compact list of active player pointers rebuilt at the start of
    // stepActors() so actor AI and damage loops skip inactive slots entirely.
    std::vector<PlayerState*> activePlayerCache_ {};
};

[[nodiscard]] std::vector<std::uint8_t> serializeInput(const InputFrame& input);
[[nodiscard]] InputFrame deserializeInput(const std::vector<std::uint8_t>& bytes);

} // namespace opm::engine
