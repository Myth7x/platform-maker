#pragma once

#include "opm/tile_metadata.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opm::engine {

inline constexpr std::uint32_t kMaxLevelDimension = 2024U;

struct TileLayer {
    std::uint32_t width {0};
    std::uint32_t height {0};
    std::vector<std::uint16_t> tileIndices {};
};

// Three rendered layers: background is drawn behind everything (decoration),
// foliage is the collision layer the player walks on, and foreground is drawn
// in front of the player (overhanging decoration). The layers always share
// the same dimensions; helpers below keep them in sync.
// Behavior script attached to an actor placement. The simulation uses this
// to pick a movement strategy each tick.
enum class ActorScript : std::uint8_t {
    MoveRandom = 0,
    MoveToPlayer = 1,
};

// Top-level "what is this actor for" classification. The simulation uses
// this to decide what happens on player overlap. Enemies damage / get
// stomped; powerups grant a style change instead and despawn.
enum class ActorCategory : std::uint8_t {
    Enemy = 0,
    Powerup = 1,
};

// A persistent actor placement saved with the level. Position is in tile
// units (bottom-left, same convention as players). The runtime spawns one
// ActorState per ActorSpawn at level load.
struct ActorSpawn {
    float x {0.0F};
    float y {0.0F};
    ActorScript script {ActorScript::MoveRandom};
    // Behavior toggles. See ActorState comments in engine.hpp for what each
    // flag does at runtime.
    bool diesWhenStomped {false};
    bool canJumpObstacles {false};
    bool canJumpRandom {false};
    bool canFly {false};
    // Visual sprite kind index. For Enemy actors this indexes into the
    // client's enemy registry (Actors/enemies/<name>/, alphabetical); for
    // Powerup actors it indexes into the powerup registry
    // (Actors/powerup/<name>/). Unknown indexes fall back to 0. The
    // simulation itself doesn't read this — purely render data.
    std::uint8_t enemyKind {0};
    ActorCategory category {ActorCategory::Enemy};
};

struct LevelData {
    TileLayer background {};
    TileLayer foliage {};
    TileLayer foreground {};
    float spawnX {0.0F};
    float spawnY {0.0F};
    float goalX {0.0F};
    float goalY {0.0F};
    std::vector<ActorSpawn> actors {};
    // Per-tile-id collision overrides. Keyed by base tile id (rotation
    // bits stripped). Tiles not in the map use their default mask from
    // collisionMaskForTile. Edited per-level via the editor's inspector.
    std::unordered_map<std::uint16_t, TileCollisionMask> tileCollisionOverrides {};
};

[[nodiscard]] LevelData createBasicLevel(std::uint32_t width, std::uint32_t height);

// Resizes all three layers to width*height. Existing tiles are preserved
// where possible (top-left aligned); new cells are zero-initialized.
void resizeAllLayers(LevelData& level, std::uint32_t width, std::uint32_t height);

// =====================================================================
// Tile rotation encoding
// ---------------------------------------------------------------------
// A tile cell is a uint16. The lower 14 bits store the tile index, the
// upper 2 bits store a rotation in 90-degree CW steps (0..3). Tile id 0
// (empty) ignores rotation. Helpers below pack/unpack.
// =====================================================================
inline constexpr std::uint16_t kTileIndexMask = 0x3FFFU;
inline constexpr std::uint16_t kTileRotationShift = 14U;

[[nodiscard]] inline std::uint16_t tileBaseIndex(std::uint16_t cell) noexcept {
    return cell & kTileIndexMask;
}
[[nodiscard]] inline std::uint8_t tileRotationSteps(std::uint16_t cell) noexcept {
    return static_cast<std::uint8_t>((cell >> kTileRotationShift) & 0x3U);
}
[[nodiscard]] inline std::uint16_t makeTileCell(std::uint16_t baseIndex, std::uint8_t rotationSteps) noexcept {
    return static_cast<std::uint16_t>((baseIndex & kTileIndexMask)
        | (static_cast<std::uint16_t>(rotationSteps & 0x3U) << kTileRotationShift));
}

// =====================================================================
// Spawn safe zone
// ---------------------------------------------------------------------
// A 5x5-tile square centered on the spawn tile. Only player actors may
// move inside it; non-player actors (enemies, powerups) are blocked from
// entering at runtime and cannot be placed inside it in the editor. The
// zone is rendered as a dotted white outline in both editor and play.
// =====================================================================
inline constexpr float kSpawnSafeZoneHalfTiles = 2.5F;

struct SpawnSafeZone {
    float minX {0.0F};
    float minY {0.0F};
    float maxX {0.0F};
    float maxY {0.0F};
};

[[nodiscard]] SpawnSafeZone computeSpawnSafeZone(const LevelData& level) noexcept;

// True iff the AABB [x, x+w] x [y, y+h] (tile units) overlaps the zone.
[[nodiscard]] bool aabbOverlapsSpawnSafeZone(const LevelData& level,
    float x, float y, float w, float h) noexcept;

} // namespace opm::engine
