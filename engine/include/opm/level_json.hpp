#pragma once

#include "opm/level.hpp"

#include <string>
#include <string_view>

namespace opm::engine {

// Serializes a LevelData to a pretty-printed JSON string.
[[nodiscard]] std::string levelToJson(const LevelData& level);

// Parses a LevelData from JSON. Returns true on success; sets `error` on failure.
// Schema (any field order accepted):
//   { "width": u32, "height": u32, "spawnX": f, "spawnY": f,
//     "goalX": f, "goalY": f, "tiles": [u16, ...] }
[[nodiscard]] bool parseLevelFromJson(std::string_view text, LevelData& out, std::string& error);

} // namespace opm::engine
