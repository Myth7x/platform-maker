#pragma once

#include "opm/assets.hpp"
#include "opm/level.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace opm::client::render {

struct TileDrawEntry {
    std::string tileAssetId {};
    std::uint32_t tileX {0};
    std::uint32_t tileY {0};
    float worldX {0.0F};
    float worldY {0.0F};
    float tileSize {0.0F};
    // Rotation in 90-degree clockwise steps (0..3). Applied at draw time
    // by rotating the texture-coordinate quadruple.
    std::uint8_t rotationSteps {0};
};

// Stable mapping from level tile index (uint16) to asset ID. Numeric stems
// (e.g. "Tiles:1") map directly to that index; non-numeric stems
// (e.g. "Tiles:top") get auto-assigned to the next free index in deterministic
// alphabetical order. Tile index 0 is always "empty".
[[nodiscard]] std::map<std::uint16_t, std::string> buildTileAssetMap(
    const opm::assets::AssetManifest& manifest);

[[nodiscard]] std::vector<TileDrawEntry> buildTileLayerDrawEntries(
    const opm::assets::AssetManifest& manifest,
    const opm::engine::TileLayer& tileLayer,
    float tileSize
);

} // namespace opm::client::render
