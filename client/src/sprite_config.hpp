#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace opm::client::sprite {

// Per-animation spec parsed from a style's config.json. Frame paths are
// stored relative to the style directory (the directory containing
// config.json).
//   fps          — cycle rate. 0 means "no cycling, frame 0 forever".
//   heightTiles  — render height in tile units. 0 means "draw at native
//                  pixel size". Each animation gets its own value so
//                  crouching can shrink the visible body without
//                  affecting other clips.
struct AnimDef {
    std::vector<std::string> leftPaths;
    std::vector<std::string> rightPaths;
    float fps {0.0F};
    float heightTiles {0.0F};
};

// Top-level config for a single player style (e.g. luigi/small).
// Render size is now driven per-animation via AnimDef::heightTiles, so
// each animation can scale to its own footprint (e.g. big-Luigi crouch
// is 1 tile while idle is 2). Unknown keys are silently ignored.
struct PlayerStyleConfig {
    // Keys: "idle", "crouch", "walk_normal", "walk_normal_turnaround",
    //       "air_normal", "walk_pspeed", "walk_pspeed_turnaround",
    //       "air_pspeed". Animations missing from the config (and from
    //       the convention folder layout) are loaded as empty clips —
    //       the runtime treats those moves as unavailable for the style.
    std::map<std::string, AnimDef> animations;
};

// Parses `<styleDir>/config.json` if it exists. On parse error, fills
// `error` and returns false. Missing file returns true with an empty
// config (caller can fall back to convention-based loading).
[[nodiscard]] bool loadPlayerStyleConfig(const std::filesystem::path& styleDir,
                                         PlayerStyleConfig& out,
                                         std::string& error);

// Per-enemy config. Same height/aspect rules as PlayerStyleConfig:
//   heightTiles == 0 → draw at native pixel size.
// `fps` controls how fast the frame list cycles when the actor is moving;
// a negative value means "leave runtime default" (6 fps).
struct EnemyConfig {
    float heightTiles {0.0F};
    float fps {-1.0F};
};

// Parses `<enemyDir>/config.json` if it exists. Same fail/missing
// semantics as loadPlayerStyleConfig.
[[nodiscard]] bool loadEnemyConfig(const std::filesystem::path& enemyDir,
                                   EnemyConfig& out,
                                   std::string& error);

} // namespace opm::client::sprite
