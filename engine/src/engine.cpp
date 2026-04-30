#include "opm/engine.hpp"
#include "opm/tile_metadata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace opm::engine {
namespace {

constexpr float kFixedDeltaSeconds = 1.0F / 60.0F;
constexpr float kWalkAcceleration = 17.5F;
constexpr float kRunAcceleration = 22.0F;
constexpr float kAirAcceleration = 8.0F;
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
constexpr float kSkidThreshold = 4.0F;
constexpr std::uint8_t kMaxJumpHoldFrames = 14U;
constexpr std::uint8_t kJumpBufferFrames = 6U;

TileCollisionMask collisionAt(const LevelData& level, const int tx, const int ty)
{
    if (tx < 0 || ty < 0) {
        return {};
    }
    if (level.groundLayer.width == 0U || level.groundLayer.height == 0U) {
        return {};
    }
    if (tx >= static_cast<int>(level.groundLayer.width) || ty >= static_cast<int>(level.groundLayer.height)) {
        return {};
    }

    const auto offset = static_cast<std::size_t>(ty) * static_cast<std::size_t>(level.groundLayer.width) + static_cast<std::size_t>(tx);
    if (offset >= level.groundLayer.tileIndices.size()) {
        return {};
    }

    return collisionMaskForTile(level.groundLayer.tileIndices[offset]);
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
    reset();
}

void Simulation::reset()
{
    state_ = {};
    if (level_.groundLayer.width == 0U || level_.groundLayer.height == 0U) {
        level_ = createBasicLevel(220U, 16U);
    }

    state_.players[0].position.x = level_.spawnX;
    state_.players[0].position.y = level_.spawnY;
    state_.players[1].position.x = level_.spawnX + 2.0F;
    state_.players[1].position.y = level_.spawnY;
}

void Simulation::step(const std::array<InputFrame, 2>& inputs)
{
    for (std::size_t playerIndex = 0; playerIndex < state_.players.size(); ++playerIndex) {
        integratePlayer(state_.players[playerIndex], inputs[playerIndex]);
    }
    state_.tick += 1;
}

void Simulation::setLevel(LevelData level)
{
    level_ = std::move(level);
    reset();
}

const GameState& Simulation::state() const
{
    return state_;
}

const LevelData& Simulation::level() const
{
    return level_;
}

void Simulation::integratePlayer(PlayerState& player, const InputFrame& input)
{
    const int moveDirection = static_cast<int>(input.moveRight) - static_cast<int>(input.moveLeft);
    const bool crouchRequested = input.crouchHeld && player.onGround;
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
        player.velocity.x += desiredDirection * accel * kFixedDeltaSeconds;
    } else {
        player.velocity.x = applyTowardsZero(player.velocity.x, friction * kFixedDeltaSeconds);
    }

    if (player.velocity.x > maxSpeed) {
        player.velocity.x = maxSpeed;
    } else if (player.velocity.x < -maxSpeed) {
        player.velocity.x = -maxSpeed;
    }

    player.skidding = false;
    if (player.onGround && effectiveMoveDirection != 0) {
        const bool opposite = (player.velocity.x > 0.0F && effectiveMoveDirection < 0) || (player.velocity.x < 0.0F && effectiveMoveDirection > 0);
        if (opposite && std::fabs(player.velocity.x) > kSkidThreshold) {
            player.skidding = true;
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

    if (player.jumpBufferFrames > 0U && player.onGround) {
        player.velocity.y = kJumpLaunchVelocity;
        player.onGround = false;
        player.jumpHoldFrames = 0U;
        player.jumpBufferFrames = 0U;
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

    float nextX = player.position.x + player.velocity.x * kFixedDeltaSeconds;
    float nextY = player.position.y;

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

    nextY += player.velocity.y * kFixedDeltaSeconds;
    player.onGround = false;

    if (player.velocity.y > 0.0F) {
        const float top = nextY + kPlayerHeightTiles;
        const int ty = static_cast<int>(std::floor(top));
        const int xStart = static_cast<int>(std::floor(nextX));
        const int xEnd = static_cast<int>(std::floor(nextX + kPlayerWidthTiles - 0.001F));
        for (int tx = xStart; tx <= xEnd; ++tx) {
            const auto mask = collisionAt(level_, tx, ty);
            if (mask.solidBottom) {
                nextY = static_cast<float>(ty) - kPlayerHeightTiles;
                player.velocity.y = 0.0F;
                break;
            }
        }
    } else if (player.velocity.y <= 0.0F) {
        const int ty = static_cast<int>(std::floor(nextY));
        const int xStart = static_cast<int>(std::floor(nextX));
        const int xEnd = static_cast<int>(std::floor(nextX + kPlayerWidthTiles - 0.001F));
        for (int tx = xStart; tx <= xEnd; ++tx) {
            const auto mask = collisionAt(level_, tx, ty);
            if (mask.solidTop) {
                nextY = static_cast<float>(ty + 1);
                player.velocity.y = 0.0F;
                player.onGround = true;
                player.jumpHoldFrames = 0U;
                break;
            }
        }
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
        hashAppendByte(hash, player.jumpHoldFrames);
        hashAppendByte(hash, player.jumpBufferFrames);
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
