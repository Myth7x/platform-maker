#pragma once

#include "opm/assets.hpp"
#include "opm/level.hpp"

#include <cstdint>
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
};

[[nodiscard]] std::vector<TileDrawEntry> buildTileLayerDrawEntries(
    const opm::assets::AssetManifest& manifest,
    const opm::engine::TileLayer& tileLayer,
    float tileSize
);

} // namespace opm::client::render
