#include "opm/engine.hpp"
#include "opm/tile_metadata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace opm::engine {
namespace {

constexpr float kFixedDeltaSeconds = 1.0F / 60.0F;
constexpr float kWalkAcceleration = 17.5F;
constexpr float kRunAcceleration = 22.0F;
constexpr float kAirAcceleration = 12.0F;
// Counter-strafe: when an airborne player presses the direction opposite to
// their current horizontal velocity, scale up air acceleration so they can
// arrest momentum and reverse mid-jump rather than gliding.
constexpr float kAirCounterStrafeMultiplier = 1.8F;
constexpr float kGroundFriction = 26.0F;
constexpr float kAirFriction = 3.0F;
constexpr float kSkidDeceleration = 34.0F;
constexpr float kMaxWalkSpeed = 7.75F;
constexpr float kMaxRunSpeed = 10.0F;
constexpr float kMaxPSpeed = 11.25F;
constexpr float kPSpeedBuildRate = 1.6F;
constexpr float kPSpeedDecayRate = 2.4F;
constexpr float kPSpeedBuildMinSpeed = 8.9F;
constexpr float kJumpLaunchVelocity = 20.75F;
constexpr float kGravityHeld = -31.0F;
constexpr float kGravityReleased = -72.0F;
constexpr float kMaxFallSpeed = -17.5F;
// Entry threshold: skidding only begins after the player is moving fast
// enough in the *opposite* direction of their input. Below this, a
// direction-tap is treated as a normal accel curve (no turnaround sprite).
constexpr float kSkidThreshold = 4.0F;
// Release threshold: once skidding, hold the state through deceleration
// and the zero-crossing until velocity has rebuilt at least this much in
// the input direction. Hysteresis with kSkidThreshold avoids flicker on
// quick input taps. Tuned so the turnaround sprite reads clearly: the
// player is visibly carried a "few units" past the reversal.
constexpr float kSkidReleaseSpeed = 2.0F;
constexpr std::uint8_t kMaxJumpHoldFrames = 14U;
constexpr std::uint8_t kJumpBufferFrames = 6U;
constexpr std::uint8_t kCoyoteFrames = 6U;
// Max horizontal overlap (in tiles) that will trigger a corner-correction nudge
// past a ceiling block instead of stopping the jump (Mario Maker 2 head-bonk
// forgiveness). 0.3 tiles ~= 7 px at 24 px/tile.
constexpr float kCornerCorrectionTiles = 0.3F;

TileCollisionMask collisionAt(const LevelData& level, const int tx, const int ty)
{
    if (tx < 0 || ty < 0) {
        return {};
    }
    if (level.foliage.width == 0U || level.foliage.height == 0U) {
        return {};
    }
    if (tx >= static_cast<int>(level.foliage.width) || ty >= static_cast<int>(level.foliage.height)) {
        return {};
    }

    const auto offset = static_cast<std::size_t>(ty) * static_cast<std::size_t>(level.foliage.width) + static_cast<std::size_t>(tx);
    if (offset >= level.foliage.tileIndices.size()) {
        return {};
    }

    const auto cell = level.foliage.tileIndices[offset];
    // Per-level override (keyed on the base tile id, rotation bits
    // stripped) wins over the global default if present.
    const auto base = tileBaseIndex(cell);
    const auto it = level.tileCollisionOverrides.find(base);
    if (it != level.tileCollisionOverrides.end()) {
        return it->second;
    }
    return collisionMaskForTile(cell);
}

float applyTowardsZero(const float value, const float amount)
{
    if (value > 0.0F) {
        return std::max(0.0F, value - amount);
    }
    if (value < 0.0F) {
        return std::min(0.0F, value + amount);
    }
    return value;
}

void hashAppendByte(std::uint64_t& hash, const std::uint8_t value)
{
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
}

void hashAppendU32(std::uint64_t& hash, const std::uint32_t value)
{
    hashAppendByte(hash, static_cast<std::uint8_t>((value >> 0U) & 0xFFU));
    hashAppendByte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    hashAppendByte(hash, static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    hashAppendByte(hash, static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void hashAppendF32(std::uint64_t& hash, const float value)
{
    hashAppendU32(hash, std::bit_cast<std::uint32_t>(value));
}

} // namespace

Simulation::Simulation()
{
    state_.players.resize(2U);
    reset();
    // Default offline behavior: slot 0 is the local player. The server
    // overrides this by deactivating every slot on startup.
    state_.players[0].active = true;
}

void Simulation::setPlayerCount(const std::size_t count)
{
    const auto target = std::max<std::size_t>(1U, count);
    state_.players.resize(target);
    // Resize default-constructs new slots (active=false). reset() preserves
    // existing active flags so callers that already configured slots keep
    // them.
    reset();
}

void Simulation::reset()
{
    if (level_.foliage.width == 0U || level_.foliage.height == 0U) {
        level_ = createBasicLevel(220U, 16U);
    }
    if (state_.players.empty()) {
        state_.players.resize(2U);
    }

    state_.tick = 0;
    for (auto& player : state_.players) {
        const bool wasActive = player.active;
        player = PlayerState {};
        player.active = wasActive;
        player.position.x = level_.spawnX;
        player.position.y = level_.spawnY;
    }

    // Spawn one runtime actor per placement saved with the level.
    state_.actors.clear();
    state_.actors.reserve(level_.actors.size());
    for (const auto& spawn : level_.actors) {
        ActorState a {};
        a.position.x = spawn.x;
        a.position.y = spawn.y;
        a.script = spawn.script;
        a.alive = true;
        a.facingRight = (spawn.script == ActorScript::MoveRandom)
            ? ((static_cast<std::uintptr_t>(state_.actors.size()) & 1U) != 0U)
            : true;
        a.diesWhenStomped = spawn.diesWhenStomped;
        a.canJumpObstacles = spawn.canJumpObstacles;
        a.canJumpRandom = spawn.canJumpRandom;
        a.canFly = spawn.canFly;
        a.enemyKind = spawn.enemyKind;
        a.category = spawn.category;
        state_.actors.push_back(a);
    }
    state_.damageEvents.clear();
}

void Simulation::step(const std::span<const InputFrame> inputs)
{
    // Snapshot start-of-tick positions so player-vs-player collisions are
    // resolved against a stable per-tick state (order-independent).
    // Reuse the persistent scratch vector to avoid a per-tick heap allocation.
    playerSnapshot_ = state_.players;
    for (std::size_t playerIndex = 0; playerIndex < state_.players.size(); ++playerIndex) {
        if (!state_.players[playerIndex].active) {
            continue;
        }
        const InputFrame input = (playerIndex < inputs.size()) ? inputs[playerIndex] : InputFrame {};
        integratePlayer(state_.players[playerIndex], input, playerSnapshot_, playerIndex);
    }
    resolvePlayerPushes();
    stepActors();
    state_.tick += 1;
}

float Simulation::sweepHorizontalAgainstTiles(const float fromX, const float targetX, const float y) const
{
    if (targetX > fromX) {
        const int tx = static_cast<int>(std::floor(targetX + kPlayerWidthTiles));
        const int yStart = static_cast<int>(std::floor(y));
        const int yEnd = static_cast<int>(std::floor(y + kPlayerHeightTiles - 0.001F));
        for (int ty = yStart; ty <= yEnd; ++ty) {
            const auto mask = collisionAt(level_, tx, ty);
            if (mask.solidLeft) {
                return std::min(targetX, static_cast<float>(tx) - kPlayerWidthTiles);
            }
        }
        return targetX;
    }
    if (targetX < fromX) {
        const int tx = static_cast<int>(std::floor(targetX));
        const int yStart = static_cast<int>(std::floor(y));
        const int yEnd = static_cast<int>(std::floor(y + kPlayerHeightTiles - 0.001F));
        for (int ty = yStart; ty <= yEnd; ++ty) {
            const auto mask = collisionAt(level_, tx, ty);
            if (mask.solidRight) {
                return std::max(targetX, static_cast<float>(tx + 1));
            }
        }
        return targetX;
    }
    return fromX;
}

void Simulation::resolvePlayerPushes()
{
    constexpr int kIterations = 4;
    // Y-overlap below this threshold means the players are stacked vertically
    // (one standing on the other's head). Don't horizontally push in that case.
    constexpr float kStackedYOverlap = 0.25F;
    constexpr float kSeparationEpsilon = 0.0005F;
    // Drag fraction: when one player is actively pushing into the other,
    // the pushed player only takes this fraction of the overlap; the pusher
    // absorbs the rest as bounce-back. With kPushedShare = 0.15 the pushed
    // player effectively moves at 15% of the pusher's velocity, so it takes
    // sustained pushing to displace someone — pushing isn't free.
    constexpr float kPushedShare = 0.15F;
    constexpr float kPusherShare = 1.0F - kPushedShare;
    // X/Y interaction-box geometry (wider than the tile-collision body so
    // contact triggers at sprite-pixel touch, not at tile-body touch).
    constexpr float kInteractXPad = (kPlayerInteractionWidthTiles - kPlayerWidthTiles) * 0.5F;

    for (int iter = 0; iter < kIterations; ++iter) {
        bool anyMoved = false;
        for (std::size_t i = 0; i < state_.players.size(); ++i) {
            if (!state_.players[i].active) {
                continue;
            }
            for (std::size_t j = i + 1; j < state_.players.size(); ++j) {
                if (!state_.players[j].active) {
                    continue;
                }
                auto& a = state_.players[i];
                auto& b = state_.players[j];

                const float aLeft = a.position.x - kInteractXPad;
                const float aRight = aLeft + kPlayerInteractionWidthTiles;
                const float bLeft = b.position.x - kInteractXPad;
                const float bRight = bLeft + kPlayerInteractionWidthTiles;
                const float aBot = a.position.y;
                const float aTop = aBot + kPlayerInteractionHeightTiles;
                const float bBot = b.position.y;
                const float bTop = bBot + kPlayerInteractionHeightTiles;

                const float xOverlap = std::min(aRight, bRight) - std::max(aLeft, bLeft);
                const float yOverlap = std::min(aTop, bTop) - std::max(aBot, bBot);
                if (xOverlap <= 0.0F || yOverlap <= 0.0F) {
                    continue;
                }
                if (yOverlap < kStackedYOverlap) {
                    continue;
                }

                const bool aIsLeft = (aLeft + aRight) < (bLeft + bRight);
                // A is "pushing" if its velocity points toward B; same for B.
                const bool aPushing = aIsLeft ? (a.velocity.x > 0.0F) : (a.velocity.x < 0.0F);
                const bool bPushing = aIsLeft ? (b.velocity.x < 0.0F) : (b.velocity.x > 0.0F);

                float aShare = 0.5F;
                float bShare = 0.5F;
                if (aPushing && !bPushing) {
                    // A is shoving B — A absorbs most of the displacement
                    // (bounces back), B barely moves.
                    aShare = kPusherShare;
                    bShare = kPushedShare;
                } else if (bPushing && !aPushing) {
                    aShare = kPushedShare;
                    bShare = kPusherShare;
                }
                // If both pushing or neither, fall through with 50/50.

                const float aPush = xOverlap * aShare + kSeparationEpsilon;
                const float bPush = xOverlap * bShare + kSeparationEpsilon;

                const float aTarget = aIsLeft ? a.position.x - aPush : a.position.x + aPush;
                const float bTarget = aIsLeft ? b.position.x + bPush : b.position.x - bPush;

                const float aClamped = sweepHorizontalAgainstTiles(a.position.x, aTarget, a.position.y);
                const float bClamped = sweepHorizontalAgainstTiles(b.position.x, bTarget, b.position.y);

                const float aSlack = std::fabs(aTarget - aClamped);
                const float bSlack = std::fabs(bTarget - bClamped);

                a.position.x = aClamped;
                b.position.x = bClamped;

                // If one side was pinned by a tile, redistribute the slack
                // onto the other player so the overlap still fully resolves.
                if (aSlack > kSeparationEpsilon) {
                    const float bExtra = aIsLeft ? b.position.x + aSlack : b.position.x - aSlack;
                    b.position.x = sweepHorizontalAgainstTiles(b.position.x, bExtra, b.position.y);
                }
                if (bSlack > kSeparationEpsilon) {
                    const float aExtra = aIsLeft ? a.position.x - bSlack : a.position.x + bSlack;
                    a.position.x = sweepHorizontalAgainstTiles(a.position.x, aExtra, a.position.y);
                }

                // If a player ended up pinned by a tile while still overlapping
                // their partner, kill the velocity component pushing into the
                // contact so they don't grind next tick.
                if (aSlack > kSeparationEpsilon) {
                    if (aIsLeft && a.velocity.x > 0.0F) {
                        a.velocity.x = 0.0F;
                        a.pSpeedMeter = 0.0F;
                        a.pSpeedActive = false;
                    } else if (!aIsLeft && a.velocity.x < 0.0F) {
                        a.velocity.x = 0.0F;
                        a.pSpeedMeter = 0.0F;
                        a.pSpeedActive = false;
                    }
                }
                if (bSlack > kSeparationEpsilon) {
                    if (aIsLeft && b.velocity.x < 0.0F) {
                        b.velocity.x = 0.0F;
                        b.pSpeedMeter = 0.0F;
                        b.pSpeedActive = false;
                    } else if (!aIsLeft && b.velocity.x > 0.0F) {
                        b.velocity.x = 0.0F;
                        b.pSpeedMeter = 0.0F;
                        b.pSpeedActive = false;
                    }
                }

                anyMoved = true;
            }
        }
        if (!anyMoved) {
            break;
        }
    }
}

void Simulation::setLevel(LevelData level)
{
    level_ = std::move(level);
    reset();
}

void Simulation::setPlayerActive(const std::size_t index, const bool active)
{
    if (index < state_.players.size()) {
        state_.players[index].active = active;
    }
}

void Simulation::respawnPlayer(const std::size_t index)
{
    if (index >= state_.players.size()) {
        return;
    }
    state_.players[index] = PlayerState {};
    state_.players[index].active = true;
    state_.players[index].position.x = level_.spawnX;
    state_.players[index].position.y = level_.spawnY;
}

const GameState& Simulation::state() const
{
    return state_;
}

GameState& Simulation::mutableState()
{
    return state_;
}

const LevelData& Simulation::level() const
{
    return level_;
}

namespace {

// Tile sweep for an actor AABB. Mirrors the player horizontal sweep but
// simpler: the actor can have its own width/height and we don't bother with
// corner correction or P-speed bookkeeping.
float sweepActorHorizontal(const LevelData& level, const float fromX, const float toX,
    const float y, const float width, const float height)
{
    if (toX > fromX) {
        const int tx = static_cast<int>(std::floor(toX + width));
        const int yStart = static_cast<int>(std::floor(y));
        const int yEnd = static_cast<int>(std::floor(y + height - 0.001F));
        for (int ty = yStart; ty <= yEnd; ++ty) {
            if (collisionAt(level, tx, ty).solidLeft) {
                return std::min(toX, static_cast<float>(tx) - width);
            }
        }
        return toX;
    }
    if (toX < fromX) {
        const int tx = static_cast<int>(std::floor(toX));
        const int yStart = static_cast<int>(std::floor(y));
        const int yEnd = static_cast<int>(std::floor(y + height - 0.001F));
        for (int ty = yStart; ty <= yEnd; ++ty) {
            if (collisionAt(level, tx, ty).solidRight) {
                return std::max(toX, static_cast<float>(tx + 1));
            }
        }
        return toX;
    }
    return fromX;
}

// Vertical sweep for an actor. Returns clamped Y and updates `landed` if it
// hit a floor. Uses solid (not one-way) tile faces only.
float sweepActorVertical(const LevelData& level, const float fromY, const float toY,
    const float x, const float width, const float height, bool& landed)
{
    landed = false;
    if (toY > fromY) {
        const int ty = static_cast<int>(std::floor(toY + height));
        const int xStart = static_cast<int>(std::floor(x));
        const int xEnd = static_cast<int>(std::floor(x + width - 0.001F));
        for (int tx = xStart; tx <= xEnd; ++tx) {
            if (collisionAt(level, tx, ty).solidBottom) {
                return std::min(toY, static_cast<float>(ty) - height);
            }
        }
        return toY;
    }
    if (toY < fromY) {
        const int ty = static_cast<int>(std::floor(toY));
        const int xStart = static_cast<int>(std::floor(x));
        const int xEnd = static_cast<int>(std::floor(x + width - 0.001F));
        for (int tx = xStart; tx <= xEnd; ++tx) {
            const auto mask = collisionAt(level, tx, ty);
            // Actors don't ride one-way platforms (keeps them grounded on
            // solid floors only — simpler behavior).
            if (mask.solidTop && !mask.oneWayTop) {
                landed = true;
                return std::max(toY, static_cast<float>(ty + 1));
            }
        }
        return toY;
    }
    return fromY;
}

// Tiny deterministic pseudo-random in [-1, 1] from (tick, actorIndex).
// Used by MoveRandom to flip direction on a schedule without a real RNG so
// server and clients (offline mode) stay deterministic.
float deterministicSign(const std::uint32_t tick, const std::size_t actorIndex)
{
    std::uint32_t h = tick * 2654435761U + static_cast<std::uint32_t>(actorIndex) * 374761393U;
    h ^= h >> 13U;
    h *= 1274126177U;
    h ^= h >> 16U;
    return ((h & 1U) != 0U) ? 1.0F : -1.0F;
}

// Compute the minimum number of "held jump" frames needed to reach a target
// peak height with launch velocity v0, held gravity gHeld, and released
// gravity gRel (gHeld and gRel given as POSITIVE magnitudes).
//
// The model mirrors the player's hold-space jump: while the actor is in
// the held phase the lighter `gHeld` is applied, and when the hold ends
// (or the actor stops rising) the heavier `gRel` is applied. Returns -1
// when even a full max-hold jump can't reach the target — caller should
// treat the obstacle as unjumpable.
int computeJumpHoldFrames(const float requiredPeak, const float v0,
    const float gHeld, const float gRel, const float dt, const int maxHold)
{
    for (int n = 0; n <= maxHold; ++n) {
        const float t = static_cast<float>(n) * dt;
        const float vH = v0 - gHeld * t; // velocity at end of held phase
        float total = 0.0F;
        if (vH <= 0.0F) {
            // Held phase already brought velocity to zero — peak reached
            // entirely during the hold. Time-to-peak = v0 / gHeld.
            const float tPeak = v0 / gHeld;
            total = v0 * tPeak - 0.5F * gHeld * tPeak * tPeak;
        } else {
            const float hHold = v0 * t - 0.5F * gHeld * t * t;
            const float hAfter = (vH * vH) / (2.0F * gRel);
            total = hHold + hAfter;
        }
        if (total >= requiredPeak) {
            return n;
        }
    }
    return -1;
}

// AI look-ahead for actors with canJumpObstacles. Scans `kLookaheadTiles`
// columns in the actor's current facing direction, looking for a solid
// wall face at body height. If one is found, walks up that column to find
// the wall's top and returns the jump peak height (in tiles above the
// actor's feet) needed to clear it, including a small safety margin.
// Returns -1 if there's nothing to jump or the wall is taller than
// `maxJumpHeight`. The jump should be triggered BEFORE the actor reaches
// the wall, so horizontal speed is preserved through the arc.
float lookAheadJumpHeight(const LevelData& level, const ActorState& actor,
    const float actorWidth, const float actorHeight, const float maxJumpHeight)
{
    if (actor.velocity.x == 0.0F) {
        return -1.0F;
    }
    const int dir = (actor.velocity.x > 0.0F) ? 1 : -1;
    const int feetTy = static_cast<int>(std::floor(actor.position.y));
    const int headTy = static_cast<int>(std::floor(actor.position.y + actorHeight - 0.001F));
    const int frontStart = (dir > 0)
        ? static_cast<int>(std::floor(actor.position.x + actorWidth - 0.001F)) + 1
        : static_cast<int>(std::floor(actor.position.x)) - 1;

    constexpr int kLookaheadTiles = 2;
    const int searchUpLimit = static_cast<int>(maxJumpHeight) + 2;

    for (int step = 0; step < kLookaheadTiles; ++step) {
        const int tx = frontStart + dir * step;

        // Wall face at body height in this column?
        bool wallHere = false;
        for (int ty = feetTy; ty <= headTy; ++ty) {
            const auto m = collisionAt(level, tx, ty);
            if ((dir > 0 && m.solidLeft) || (dir < 0 && m.solidRight)) {
                wallHere = true;
                break;
            }
        }
        if (!wallHere) {
            continue;
        }

        // Find the top of the wall — first row above headTy where the
        // facing-side face is no longer solid.
        for (int up = 1; up <= searchUpLimit; ++up) {
            const int ty = headTy + up;
            const auto m = collisionAt(level, tx, ty);
            const bool stillSolid = (dir > 0) ? m.solidLeft : m.solidRight;
            if (!stillSolid) {
                // Tile (tx, ty-1) is the topmost wall tile; its top edge is at y = ty.
                const float requiredPeak =
                    static_cast<float>(ty - feetTy) + 0.4F; // +safety margin
                if (requiredPeak > maxJumpHeight) {
                    return -1.0F;
                }
                return requiredPeak;
            }
        }
        return -1.0F; // wall taller than search limit
    }
    return -1.0F;
}

} // namespace

void Simulation::stepActors()
{
    state_.damageEvents.clear();
    if (state_.actors.empty()) {
        return;
    }

    constexpr float kActorWidth = 0.9F;        // tiles
    constexpr float kActorHeight = 1.0F;       // tiles
    constexpr float kActorRandomSpeed = 3.0F;  // tiles/sec
    constexpr float kActorChaseSpeed = 4.5F;   // tiles/sec
    constexpr float kActorGravity = -45.0F;    // tiles/sec^2 (released)
    // Lighter gravity applied while jumpHoldFrames > 0 and vy > 0. Same
    // hold/release ratio the player uses, so a max-held actor jump tops
    // out at ~6 tiles peak — enough headroom for 4-tile-tall walls plus
    // margin and enough airtime to cross wide platform tops.
    constexpr float kActorGravityHeld = -31.0F;

    // Pre-build a compact list of active player pointers so MoveToPlayer
    // actors scan only active slots instead of iterating 500 entries per actor.
    activePlayerCache_.clear();
    for (auto& p : state_.players) {
        if (p.active) {
            activePlayerCache_.push_back(&p);
        }
    }
    constexpr float kActorMaxFall = -16.0F;
    constexpr float kActorJumpLaunchVelocity = 20.75F; // matches player launch
    constexpr float kActorJumpVelocity = 16.0F;        // simple impulse for canJumpRandom
    constexpr std::uint8_t kActorMaxJumpHold = 14U;
    constexpr std::uint16_t kRandomDirChangeFrames = 90U; // 1.5s @ 60Hz
    constexpr std::uint16_t kRandomJumpFrames = 96U;      // ~1.6s between jumps
    constexpr float kFlySpeed = 4.0F;                     // canFly chase Y speed
    // Walls of this height (tiles) are still considered jumpable. Above
    // this the AI gives up and the actor turns around at the wall.
    constexpr float kMaxJumpableTiles = 5.5F;

    for (std::size_t i = 0; i < state_.actors.size(); ++i) {
        auto& actor = state_.actors[i];
        if (!actor.alive) {
            continue;
        }

        // --- Choose horizontal velocity from script (and remember the
        //     player target for canFly chase-Y handling below).
        const PlayerState* target = nullptr;
        if (actor.script == ActorScript::MoveRandom) {
            if (actor.scriptTimer == 0U) {
                actor.facingRight = (deterministicSign(state_.tick, i) > 0.0F);
                actor.scriptTimer = kRandomDirChangeFrames;
            } else {
                actor.scriptTimer = static_cast<std::uint16_t>(actor.scriptTimer - 1U);
            }
            actor.velocity.x = (actor.facingRight ? 1.0F : -1.0F) * kActorRandomSpeed;
        } else { // MoveToPlayer
            float bestDist = std::numeric_limits<float>::infinity();
            for (PlayerState* p : activePlayerCache_) {
                const float d = std::fabs(p->position.x - actor.position.x);
                if (d < bestDist) {
                    bestDist = d;
                    target = p;
                }
            }
            if (target != nullptr) {
                const float dx = target->position.x - actor.position.x;
                if (std::fabs(dx) < 0.05F) {
                    actor.velocity.x = 0.0F;
                } else {
                    actor.facingRight = dx > 0.0F;
                    actor.velocity.x = (actor.facingRight ? 1.0F : -1.0F) * kActorChaseSpeed;
                }
            } else {
                actor.velocity.x = 0.0F;
            }
        }

        // --- Predictive obstacle-jump (look-ahead AI) ---
        // Run BEFORE gravity application so the launch impulse and the
        // first held-gravity tick happen on the same frame. The AI scans a
        // couple of tiles in the facing direction; if it finds a clearable
        // wall it computes the minimum hold time needed to peak just above
        // the top, then "presses jump" with kActorJumpLaunchVelocity and
        // arms the hold counter. Horizontal velocity is preserved, so the
        // arc carries the actor over without face-planting and losing speed.
        if (actor.canJumpObstacles && !actor.canFly && actor.onGround) {
            const float peak = lookAheadJumpHeight(
                level_, actor, kActorWidth, kActorHeight, kMaxJumpableTiles);
            if (peak > 0.0F) {
                const int hold = computeJumpHoldFrames(
                    peak,
                    kActorJumpLaunchVelocity,
                    std::fabs(kActorGravityHeld),
                    std::fabs(kActorGravity),
                    kFixedDeltaSeconds,
                    static_cast<int>(kActorMaxJumpHold));
                if (hold >= 0) {
                    actor.velocity.y = kActorJumpLaunchVelocity;
                    actor.jumpHoldFrames = static_cast<std::uint8_t>(hold);
                    actor.onGround = false;
                }
            }
        }

        // --- Vertical drive: fly (no gravity, optional Y chase) vs gravity ---
        if (actor.canFly) {
            if (actor.script == ActorScript::MoveToPlayer && target != nullptr) {
                const float dy = target->position.y - actor.position.y;
                if (std::fabs(dy) < 0.05F) {
                    actor.velocity.y = 0.0F;
                } else {
                    actor.velocity.y = (dy > 0.0F ? 1.0F : -1.0F) * kFlySpeed;
                }
            } else {
                actor.velocity.y = 0.0F;
            }
            actor.onGround = false;
            // Flying actors don't use the held-jump mechanic.
            actor.jumpHoldFrames = 0U;
        } else {
            // Lighter gravity while a hold is armed and the actor is still
            // rising (matches player jump feel). Once the hold expires or
            // the apex passes, the heavier release gravity takes over.
            const float gravity = (actor.jumpHoldFrames > 0U && actor.velocity.y > 0.0F)
                ? kActorGravityHeld : kActorGravity;
            actor.velocity.y += gravity * kFixedDeltaSeconds;
            if (actor.jumpHoldFrames > 0U) {
                actor.jumpHoldFrames = static_cast<std::uint8_t>(actor.jumpHoldFrames - 1U);
            }
            if (actor.velocity.y < kActorMaxFall) {
                actor.velocity.y = kActorMaxFall;
            }
        }

        // --- Random-jump check (deterministic schedule) ---
        if (actor.canJumpRandom && !actor.canFly && actor.onGround) {
            if (actor.jumpCooldown == 0U) {
                if (deterministicSign(state_.tick + 7919U, i) > 0.0F) {
                    actor.velocity.y = kActorJumpVelocity;
                    actor.onGround = false;
                }
                actor.jumpCooldown = kRandomJumpFrames;
            }
        }
        if (actor.jumpCooldown > 0U) {
            actor.jumpCooldown = static_cast<std::uint16_t>(actor.jumpCooldown - 1U);
        }

        // --- Horizontal sweep ---
        const float desiredNextX = actor.position.x + actor.velocity.x * kFixedDeltaSeconds;
        float nextX = sweepActorHorizontal(level_, actor.position.x, desiredNextX,
            actor.position.y, kActorWidth, kActorHeight);
        // Spawn safe zone acts like a solid wall for non-player actors.
        // Only clamp when the actor was already outside on this axis — if
        // it somehow starts inside (e.g., spawn was moved on top of it)
        // we let it walk out instead of pinning it.
        {
            const SpawnSafeZone z = computeSpawnSafeZone(level_);
            // Only relevant when the actor's Y span overlaps the zone's Y span.
            const bool yOverlap = (actor.position.y + kActorHeight) > z.minY
                && actor.position.y < z.maxY;
            if (yOverlap) {
                if (actor.velocity.x > 0.0F
                    && actor.position.x + kActorWidth <= z.minX + 0.0001F
                    && nextX + kActorWidth > z.minX) {
                    nextX = z.minX - kActorWidth;
                } else if (actor.velocity.x < 0.0F
                    && actor.position.x >= z.maxX - 0.0001F
                    && nextX < z.maxX) {
                    nextX = z.maxX;
                }
            }
        }
        const bool blockedByWall =
            std::fabs(nextX - desiredNextX) > 0.0001F
            && actor.velocity.x != 0.0F;
        if (blockedByWall) {
            // Either the lookahead found the wall too tall, the safe zone
            // refused entry, or the actor can't jump at all. Flip direction
            // so the actor doesn't grind.
            actor.facingRight = !actor.facingRight;
            actor.velocity.x = -actor.velocity.x;
        }

        // --- Vertical sweep ---
        bool landed = false;
        const float desiredNextY = actor.position.y + actor.velocity.y * kFixedDeltaSeconds;
        float nextY = sweepActorVertical(level_, actor.position.y, desiredNextY,
            nextX, kActorWidth, kActorHeight, landed);
        // Vertical safe-zone clamp (mainly affects flying actors entering
        // from above/below; ground enemies are usually stopped by the
        // horizontal clamp first).
        {
            const SpawnSafeZone z = computeSpawnSafeZone(level_);
            const bool xOverlap = (nextX + kActorWidth) > z.minX && nextX < z.maxX;
            if (xOverlap) {
                if (actor.velocity.y > 0.0F
                    && actor.position.y + kActorHeight <= z.minY + 0.0001F
                    && nextY + kActorHeight > z.minY) {
                    nextY = z.minY - kActorHeight;
                    actor.velocity.y = 0.0F;
                } else if (actor.velocity.y < 0.0F
                    && actor.position.y >= z.maxY - 0.0001F
                    && nextY < z.maxY) {
                    nextY = z.maxY;
                    if (!actor.canFly) {
                        landed = true;
                    } else {
                        actor.velocity.y = 0.0F;
                    }
                }
            }
        }
        if (landed) {
            actor.velocity.y = 0.0F;
            actor.onGround = true;
        } else if (!actor.canFly) {
            actor.onGround = false;
        }

        actor.position.x = nextX;
        actor.position.y = nextY;
    }

    // --- Actor-vs-player damage check ---
    // AABB overlap → mark damage event and respawn the player at the level
    // spawn (clearing velocity). The actor is unaffected.
    for (std::size_t ai = 0; ai < state_.actors.size(); ++ai) {
        auto& actor = state_.actors[ai];
        if (!actor.alive) {
            continue;
        }
        const float aLeft = actor.position.x;
        const float aRight = aLeft + 0.9F; // matches kActorWidth
        const float aBot = actor.position.y;
        const float aTop = aBot + 1.0F;    // matches kActorHeight

        for (std::size_t pi = 0; pi < activePlayerCache_.size(); ++pi) {
            auto& p = *activePlayerCache_[pi];
            const std::size_t playerIndex = static_cast<std::size_t>(
                activePlayerCache_[pi] - state_.players.data());
            const float pLeft = p.position.x;
            const float pRight = pLeft + kPlayerWidthTiles;
            const float pBot = p.position.y;
            const float pTop = pBot + kPlayerHeightTiles;
            if (pRight <= aLeft || pLeft >= aRight) {
                continue;
            }
            if (pTop <= aBot || pBot >= aTop) {
                continue;
            }

            // ----- Powerup branch -----
            // Powerups never damage. On overlap the actor despawns and
            // the player's style is upgraded one rung (Small -> Big).
            // Players already at Big-or-higher don't consume the powerup;
            // it stays in the world. (We could also play a "double-up"
            // SFX later — a no-op for now keeps the placeholder simple.)
            if (actor.category == ActorCategory::Powerup) {
                if (p.style == PlayerStyle::Small) {
                    p.style = PlayerStyle::Big;
                    actor.alive = false;
                    // Mario-style upgrade animation: freeze the player +
                    // grant invincibility for the duration. The renderer
                    // flashes between styles while the counter is alive.
                    constexpr std::uint8_t kPowerupTransitionFrames = 36U; // 0.6s @ 60Hz
                    p.powerupTransitionFrames = kPowerupTransitionFrames;
                    ActorDamageEvent ev {};
                    ev.actorIndex = static_cast<std::uint16_t>(ai);
                    ev.playerIndex = static_cast<std::uint16_t>(playerIndex);
                    state_.damageEvents.push_back(ev);
                    break;
                }
                continue;
            }

            // ----- Enemy branch -----
            // While the player has any active invulnerability — either
            // mid power-up transition or post-damage i-frames — enemy
            // contact does nothing (no stomp, no damage).
            if (p.powerupTransitionFrames > 0U || p.invincibilityFrames > 0U) {
                continue;
            }

            // Stomp-kill takes priority: if the actor allows it AND the
            // player is descending onto the actor's head, kill the actor
            // and bounce the player. Tolerance lets a slight overlap still
            // register as a stomp.
            const bool isStomp = actor.diesWhenStomped
                && p.velocity.y <= 0.0F
                && pBot >= aTop - 0.4F;
            if (isStomp) {
                actor.alive = false;
                p.velocity.y = 13.0F;
                p.onGround = false;
                p.jumpHoldFrames = 0U;
                ActorDamageEvent ev {};
                ev.actorIndex = static_cast<std::uint16_t>(ai);
                ev.playerIndex = static_cast<std::uint16_t>(playerIndex);
                state_.damageEvents.push_back(ev);
                break;
            }

            // Non-stomp enemy contact damages the player.
            //   Big  -> downgrade to Small in place + grant i-frames so
            //           the player can recover and the renderer blinks
            //           the sprite.
            //   Small -> full death: respawn at the level spawn with
            //            state reset.
            ActorDamageEvent ev {};
            ev.actorIndex = static_cast<std::uint16_t>(ai);
            ev.playerIndex = static_cast<std::uint16_t>(playerIndex);
            state_.damageEvents.push_back(ev);

            if (p.style == PlayerStyle::Big) {
                p.style = PlayerStyle::Small;
                // ~1.5 s @ 60 Hz of post-damage invincibility, matching
                // the classic Mario "blink and walk away" recovery.
                constexpr std::uint8_t kPostDamageIFrames = 90U;
                p.invincibilityFrames = kPostDamageIFrames;
                continue;
            }

            const bool wasActive = p.active;
            p = PlayerState {};
            p.active = wasActive;
            p.position.x = level_.spawnX;
            p.position.y = level_.spawnY;
        }
    }
}

void Simulation::integratePlayer(PlayerState& player, const InputFrame& input,
    const std::vector<PlayerState>& others, const std::size_t selfIndex)
{
    // Power-up transition: freeze the player in place, ignore input, zero
    // velocity, and bail out before any movement / collision logic. The
    // damage check in stepActors also skips enemy contact while this
    // counter is active so the player can't be hit during the upgrade
    // animation. The renderer reads the same counter to drive its
    // style-flicker.
    if (player.powerupTransitionFrames > 0U) {
        player.velocity.x = 0.0F;
        player.velocity.y = 0.0F;
        player.skidding = false;
        player.crouching = false;
        player.jumpBufferFrames = 0U;
        player.coyoteFrames = 0U;
        player.powerupTransitionFrames =
            static_cast<std::uint8_t>(player.powerupTransitionFrames - 1U);
        (void)others;
        (void)selfIndex;
        return;
    }
    // Post-damage i-frames just count down; the player keeps full control
    // and full physics, only enemy contact is suppressed (handled in
    // stepActors). Decrementing here ensures it ticks even if no damage
    // event happens this frame.
    if (player.invincibilityFrames > 0U) {
        player.invincibilityFrames =
            static_cast<std::uint8_t>(player.invincibilityFrames - 1U);
    }

    const int moveDirection = static_cast<int>(input.moveRight) - static_cast<int>(input.moveLeft);
    // A style can opt out of crouching: small Luigi has no crouch sprite,
    // so the move is disabled while he's small. Big Luigi (and any future
    // style that ships a crouch animation) crouches normally.
    const bool styleAllowsCrouch = (player.style != PlayerStyle::Small);
    const bool crouchRequested = input.crouchHeld && player.onGround && styleAllowsCrouch;
    const int effectiveMoveDirection = crouchRequested ? 0 : moveDirection;
    const float desiredDirection = static_cast<float>(effectiveMoveDirection);

    if (effectiveMoveDirection > 0) {
        player.facingRight = true;
    } else if (effectiveMoveDirection < 0) {
        player.facingRight = false;
    }

    const float maxSpeed = input.runHeld ? (player.pSpeedActive ? kMaxPSpeed : kMaxRunSpeed) : kMaxWalkSpeed;
    const float accel = player.onGround ? (input.runHeld ? kRunAcceleration : kWalkAcceleration) : kAirAcceleration;
    const float friction = player.onGround ? kGroundFriction : kAirFriction;

    if (effectiveMoveDirection != 0) {
        const bool opposite = (player.velocity.x > 0.0F && effectiveMoveDirection < 0) || (player.velocity.x < 0.0F && effectiveMoveDirection > 0);
        if (player.onGround && opposite) {
            player.velocity.x = applyTowardsZero(player.velocity.x, kSkidDeceleration * kFixedDeltaSeconds);
        }
        const float effectiveAccel = (!player.onGround && opposite)
            ? accel * kAirCounterStrafeMultiplier
            : accel;
        player.velocity.x += desiredDirection * effectiveAccel * kFixedDeltaSeconds;
    } else {
        player.velocity.x = applyTowardsZero(player.velocity.x, friction * kFixedDeltaSeconds);
    }

    if (player.velocity.x > maxSpeed) {
        player.velocity.x = maxSpeed;
    } else if (player.velocity.x < -maxSpeed) {
        player.velocity.x = -maxSpeed;
    }

    // Skidding (turnaround sprite) state machine.
    //
    // ENTER: airborne -> grounded with input opposite to motion AND speed
    //        above kSkidThreshold. Below the threshold a brief direction
    //        tap doesn't engage the turnaround pose.
    //
    // STAY:  through the deceleration phase and across the zero-crossing.
    //        We deliberately do NOT clear the flag while velocity drops
    //        below kSkidThreshold or even reaches zero — the sprite reads
    //        as "still turning around" until the player has visibly built
    //        up speed in the new direction.
    //
    // EXIT:  player leaves the ground, releases their input, OR velocity
    //        is aligned with input AND |vx| has rebuilt past
    //        kSkidReleaseSpeed (so they've moved a few units in the new
    //        direction). Hysteresis between the two thresholds prevents
    //        flicker on quick input taps.
    {
        const bool inputOpposite = effectiveMoveDirection != 0
            && ((player.velocity.x > 0.0F && effectiveMoveDirection < 0)
             || (player.velocity.x < 0.0F && effectiveMoveDirection > 0));
        if (player.onGround && inputOpposite
            && std::fabs(player.velocity.x) > kSkidThreshold) {
            player.skidding = true;
        }
        if (player.skidding) {
            const bool inputAligned = effectiveMoveDirection != 0
                && ((player.velocity.x > 0.0F && effectiveMoveDirection > 0)
                 || (player.velocity.x < 0.0F && effectiveMoveDirection < 0));
            const bool releaseByInput   = effectiveMoveDirection == 0;
            const bool releaseByLeaving = !player.onGround;
            const bool releaseByRebuild = inputAligned
                && std::fabs(player.velocity.x) > kSkidReleaseSpeed;
            if (releaseByLeaving || releaseByInput || releaseByRebuild) {
                player.skidding = false;
            }
        }
    }

    const bool pSpeedInterrupted = crouchRequested || player.skidding;
    const bool pSpeedBuilding =
        player.onGround &&
        input.runHeld &&
        !pSpeedInterrupted &&
        effectiveMoveDirection != 0 &&
        std::fabs(player.velocity.x) >= kPSpeedBuildMinSpeed;

    const bool atMaxPSpeed = player.pSpeedMeter >= 0.999F;
    const bool movingHorizontally = std::fabs(player.velocity.x) > 0.05F;
    const bool preserveAirPSpeed = !player.onGround && atMaxPSpeed && movingHorizontally;

    if (pSpeedInterrupted) {
        player.pSpeedMeter = 0.0F;
        player.pSpeedActive = false;
    } else if (pSpeedBuilding) {
        player.pSpeedMeter += kPSpeedBuildRate * kFixedDeltaSeconds;
    } else if (!preserveAirPSpeed) {
        player.pSpeedMeter -= kPSpeedDecayRate * kFixedDeltaSeconds;
    }

    player.pSpeedMeter = std::clamp(player.pSpeedMeter, 0.0F, 1.0F);
    if (player.pSpeedMeter >= 0.999F) {
        player.pSpeedActive = true;
    }
    if (player.pSpeedMeter <= 0.001F) {
        player.pSpeedActive = false;
    }

    player.crouching = crouchRequested;

    if (input.jumpPressed) {
        player.jumpBufferFrames = kJumpBufferFrames;
    } else if (player.jumpBufferFrames > 0U) {
        player.jumpBufferFrames = static_cast<std::uint8_t>(player.jumpBufferFrames - 1U);
    }

    if (player.jumpBufferFrames > 0U && (player.onGround || player.coyoteFrames > 0U)) {
        player.velocity.y = kJumpLaunchVelocity;
        player.onGround = false;
        player.jumpHoldFrames = 0U;
        player.jumpBufferFrames = 0U;
        player.coyoteFrames = 0U;
    }

    const bool rising = player.velocity.y > 0.0F;
    const bool canSustainJump = input.jumpHeld && rising && player.jumpHoldFrames < kMaxJumpHoldFrames;
    const float gravity = canSustainJump ? kGravityHeld : kGravityReleased;
    if (canSustainJump) {
        player.jumpHoldFrames = static_cast<std::uint8_t>(player.jumpHoldFrames + 1U);
    }
    player.velocity.y += gravity * kFixedDeltaSeconds;
    if (player.velocity.y < kMaxFallSpeed) {
        player.velocity.y = kMaxFallSpeed;
    }

    const float prevBottomY = player.position.y;

    float nextX = player.position.x + player.velocity.x * kFixedDeltaSeconds;
    float nextY = player.position.y;

    // --- Horizontal sweep ---------------------------------------------------
    // Note: solidLeft/solidRight are only set on full-block tiles, so one-way
    // semi-solids (oneWayTop only) naturally pass through here.
    if (player.velocity.x > 0.0F) {
        const float right = nextX + kPlayerWidthTiles;
        const int tx = static_cast<int>(std::floor(right));
        const int yStart = static_cast<int>(std::floor(player.position.y));
        const int yEnd = static_cast<int>(std::floor(player.position.y + kPlayerHeightTiles - 0.001F));
        for (int ty = yStart; ty <= yEnd; ++ty) {
            const auto mask = collisionAt(level_, tx, ty);
            if (mask.solidLeft) {
                nextX = static_cast<float>(tx) - kPlayerWidthTiles;
                player.velocity.x = 0.0F;
                player.pSpeedMeter = 0.0F;
                player.pSpeedActive = false;
                break;
            }
        }
    } else if (player.velocity.x < 0.0F) {
        const int tx = static_cast<int>(std::floor(nextX));
        const int yStart = static_cast<int>(std::floor(player.position.y));
        const int yEnd = static_cast<int>(std::floor(player.position.y + kPlayerHeightTiles - 0.001F));
        for (int ty = yStart; ty <= yEnd; ++ty) {
            const auto mask = collisionAt(level_, tx, ty);
            if (mask.solidRight) {
                nextX = static_cast<float>(tx + 1);
                player.velocity.x = 0.0F;
                player.pSpeedMeter = 0.0F;
                player.pSpeedActive = false;
                break;
            }
        }
    }

    // Horizontal player-vs-player collision is resolved AFTER all players
    // integrate, in Simulation::resolvePlayerPushes(), so that side contact
    // mutually displaces both players (push behavior) instead of acting like
    // a wall.

    // --- Vertical sweep -----------------------------------------------------
    nextY += player.velocity.y * kFixedDeltaSeconds;
    player.onGround = false;

    if (player.velocity.y > 0.0F) {
        // Rising: ceiling check on the top edge. Apply MM2-style corner
        // correction when only one of the two spanned columns blocks and the
        // horizontal overlap with that column is small.
        const float top = nextY + kPlayerHeightTiles;
        const int ty = static_cast<int>(std::floor(top));
        const int xStart = static_cast<int>(std::floor(nextX));
        const int xEnd = static_cast<int>(std::floor(nextX + kPlayerWidthTiles - 0.001F));

        const auto leftMask = collisionAt(level_, xStart, ty);
        const auto rightMask = (xEnd > xStart) ? collisionAt(level_, xEnd, ty) : leftMask;
        const bool leftBlocks = leftMask.solidBottom; // oneWayTop tiles do not have solidBottom
        const bool rightBlocks = (xEnd > xStart) && rightMask.solidBottom;

        if (leftBlocks && !rightBlocks && xEnd > xStart) {
            // Player straddles two columns; only the left one is solid.
            // Try to nudge right so the player slips past the corner.
            const float overlap = (static_cast<float>(xStart + 1)) - nextX;
            if (overlap > 0.0F && overlap <= kCornerCorrectionTiles) {
                nextX = static_cast<float>(xStart + 1);
            } else {
                nextY = static_cast<float>(ty) - kPlayerHeightTiles;
                player.velocity.y = 0.0F;
            }
        } else if (rightBlocks && !leftBlocks) {
            const float overlap = (nextX + kPlayerWidthTiles) - static_cast<float>(xEnd);
            if (overlap > 0.0F && overlap <= kCornerCorrectionTiles) {
                nextX = static_cast<float>(xEnd) - kPlayerWidthTiles;
            } else {
                nextY = static_cast<float>(ty) - kPlayerHeightTiles;
                player.velocity.y = 0.0F;
            }
        } else if (leftBlocks || rightBlocks) {
            nextY = static_cast<float>(ty) - kPlayerHeightTiles;
            player.velocity.y = 0.0F;
        }
    } else if (player.velocity.y <= 0.0F) {
        // Falling (or grounded probe with vy==0): floor check on the bottom
        // edge. One-way (semi-solid) platforms only block when the player's
        // previous bottom was at or above the tile's top edge.
        const int ty = static_cast<int>(std::floor(nextY));
        const int xStart = static_cast<int>(std::floor(nextX));
        const int xEnd = static_cast<int>(std::floor(nextX + kPlayerWidthTiles - 0.001F));
        for (int tx = xStart; tx <= xEnd; ++tx) {
            const auto mask = collisionAt(level_, tx, ty);
            if (!mask.solidTop) {
                continue;
            }
            const float tileTopY = static_cast<float>(ty + 1);
            if (mask.oneWayTop && prevBottomY < tileTopY - 0.0001F) {
                // Player was below the platform's top edge before this step,
                // so they're rising up through it (or already overlapping
                // from below). Don't snap them onto it.
                continue;
            }
            nextY = tileTopY;
            player.velocity.y = 0.0F;
            player.onGround = true;
            player.jumpHoldFrames = 0U;
            break;
        }
    }

    // --- Player-vs-player vertical collision --------------------------------
    // Other active players act as one-way platforms (semi-solid): the player
    // can land on top of another's head, but rises freely through them from
    // below. This matches the tile semi-solid rule and avoids ceiling-bonking
    // a teammate.
    if (player.velocity.y <= 0.0F) {
        constexpr float kInteractXPad = (kPlayerInteractionWidthTiles - kPlayerWidthTiles) * 0.5F;
        for (std::size_t i = 0; i < others.size(); ++i) {
            if (i == selfIndex) {
                continue;
            }
            const auto& o = others[i];
            if (!o.active) {
                continue;
            }
            // X-overlap and head-top use the wider interaction box so contact
            // happens when the rendered sprites visually meet, not when the
            // tighter tile-collision bodies overlap.
            const float oLeft = o.position.x - kInteractXPad;
            const float oRight = oLeft + kPlayerInteractionWidthTiles;
            const float oTop = o.position.y + kPlayerInteractionHeightTiles;

            const float pLeft = nextX - kInteractXPad;
            const float pRight = pLeft + kPlayerInteractionWidthTiles;
            if (pRight <= oLeft + 0.0001F || pLeft >= oRight - 0.0001F) {
                continue;
            }
            if (prevBottomY < oTop - 0.0001F) {
                continue;
            }
            if (nextY >= oTop - 0.0001F) {
                continue;
            }
            nextY = oTop;
            player.velocity.y = 0.0F;
            player.onGround = true;
            player.jumpHoldFrames = 0U;
        }
    }

    // --- Coyote time --------------------------------------------------------
    // While grounded, keep the coyote window topped up. Once airborne it
    // ticks down; the buffered-jump check at the top of next frame consumes
    // it as needed.
    if (player.onGround) {
        player.coyoteFrames = kCoyoteFrames;
    } else if (player.coyoteFrames > 0U) {
        player.coyoteFrames = static_cast<std::uint8_t>(player.coyoteFrames - 1U);
    }

    if (!player.onGround) {
        player.crouching = false;
    }

    if (player.onGround && player.jumpBufferFrames > 0U) {
        player.velocity.y = kJumpLaunchVelocity;
        player.onGround = false;
        player.jumpHoldFrames = 0U;
        player.jumpBufferFrames = 0U;
        player.crouching = false;
    }

    player.position.x = nextX;
    player.position.y = nextY;
}

std::uint64_t Simulation::stateHash() const
{
    std::uint64_t hash = 1469598103934665603ULL;

    hashAppendU32(hash, state_.tick);
    for (const auto& player : state_.players) {
        hashAppendF32(hash, player.position.x);
        hashAppendF32(hash, player.position.y);
        hashAppendF32(hash, player.velocity.x);
        hashAppendF32(hash, player.velocity.y);
        hashAppendByte(hash, static_cast<std::uint8_t>(player.onGround ? 1U : 0U));
        hashAppendByte(hash, static_cast<std::uint8_t>(player.facingRight ? 1U : 0U));
        hashAppendByte(hash, static_cast<std::uint8_t>(player.skidding ? 1U : 0U));
        hashAppendByte(hash, static_cast<std::uint8_t>(player.crouching ? 1U : 0U));
        hashAppendByte(hash, static_cast<std::uint8_t>(player.pSpeedActive ? 1U : 0U));
        hashAppendByte(hash, static_cast<std::uint8_t>(player.active ? 1U : 0U));
        hashAppendByte(hash, static_cast<std::uint8_t>(player.style));
        hashAppendByte(hash, player.powerupTransitionFrames);
        hashAppendByte(hash, player.invincibilityFrames);
        hashAppendByte(hash, player.jumpHoldFrames);
        hashAppendByte(hash, player.jumpBufferFrames);
        hashAppendByte(hash, player.coyoteFrames);
        hashAppendF32(hash, player.pSpeedMeter);
    }

    return hash;
}

std::vector<std::uint8_t> serializeInput(const InputFrame& input)
{
    std::vector<std::uint8_t> bytes(sizeof(InputFrame));
    std::memcpy(bytes.data(), &input, sizeof(InputFrame));
    return bytes;
}

InputFrame deserializeInput(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() != sizeof(InputFrame)) {
        throw std::runtime_error("InputFrame payload has invalid size");
    }

    InputFrame input {};
    std::memcpy(&input, bytes.data(), sizeof(InputFrame));
    return input;
}

} // namespace opm::engine
