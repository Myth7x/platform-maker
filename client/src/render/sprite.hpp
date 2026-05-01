#pragma once

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include "render/texture.hpp"

#include "opm/engine.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace opm::client::render {

struct AnimClip {
    // [right, left] frame lists. Either side may be empty: the selector
    // falls back to the populated side if so.
    std::vector<Texture2D> framesRight;
    std::vector<Texture2D> framesLeft;
    // Frames-per-second cycling rate. 0 means "static — show frame 0 only".
    float fps {0.0F};
    // Render height in tile units for this clip. 0 means "draw at native
    // pixel size".
    float heightTiles {0.0F};

    [[nodiscard]] bool empty() const noexcept {
        return framesRight.empty() && framesLeft.empty();
    }
    [[nodiscard]] const std::vector<Texture2D>& side(bool right) const noexcept {
        const auto& primary = right ? framesRight : framesLeft;
        if (!primary.empty()) {
            return primary;
        }
        return right ? framesLeft : framesRight;
    }
};

struct PlayerSprite {
    AnimClip idle;
    AnimClip crouch;
    AnimClip walkNormal;
    AnimClip walkNormalTurnaround;
    AnimClip airNormal;
    AnimClip walkPSpeed;
    AnimClip walkPSpeedTurnaround;
    AnimClip airPSpeed;
    bool ready {false};
};

// Holds one PlayerSprite per PlayerStyle. The renderer indexes into this
// by `playerState.style` to pick which sprite to draw.
struct PlayerSpriteSet {
    PlayerSprite small;
    PlayerSprite big;

    [[nodiscard]] const PlayerSprite& forStyle(opm::engine::PlayerStyle style) const noexcept {
        switch (style) {
            case opm::engine::PlayerStyle::Big:   return big.ready ? big : small;
            case opm::engine::PlayerStyle::Small:
            default:                              return small;
        }
    }
};

struct EnemySprite {
    std::vector<Texture2D> frames;
    // Render height in tile units. Width derives from each frame's
    // texture aspect ratio. 0 means "draw at native pixel size".
    float heightTiles {0.0F};
    // Frames per second when the actor is moving. <= 0 falls back to the
    // hard-coded default (6 fps). Held still, the sprite is frame 0.
    float fps {-1.0F};
    bool ready {false};
};

// Catalog of all enemy sprite directories under Actors/enemies/, sorted
// alphabetically. Levels and the wire protocol store actors by their kind
// index into this list.
struct EnemyRegistry {
    std::vector<std::string> names;
    std::vector<EnemySprite> sprites;

    [[nodiscard]] std::size_t size() const noexcept { return sprites.size(); }
    [[nodiscard]] const EnemySprite* spriteFor(std::uint8_t kind) const noexcept {
        if (sprites.empty()) {
            return nullptr;
        }
        const auto idx = (kind < sprites.size()) ? kind : 0U;
        return sprites[idx].ready ? &sprites[idx] : nullptr;
    }
};

PlayerSprite loadPlayerSprite(const std::filesystem::path& styleDir);
EnemySprite  loadEnemySprite(const std::filesystem::path& dir);
EnemyRegistry loadEnemyRegistry(const std::filesystem::path& enemiesRoot);

// Result of picking the right frame for a player this tick.
struct PlayerFrameSelection {
    const Texture2D* tex {nullptr};
    const AnimClip* clip {nullptr};
};

PlayerFrameSelection selectPlayerFrame(const PlayerSprite& sprite,
                                       const opm::engine::PlayerState& player,
                                       float animTime);

const Texture2D* selectEnemyFrame(const EnemySprite& sprite,
                                  float animTime,
                                  float speedHint);

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
