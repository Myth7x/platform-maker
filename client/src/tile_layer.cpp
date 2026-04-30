#include "tile_layer.hpp"

#include <charconv>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opm::client::render {

std::vector<TileDrawEntry> buildTileLayerDrawEntries(
    const opm::assets::AssetManifest& manifest,
    const opm::engine::TileLayer& tileLayer,
    const float tileSize
)
{
    std::unordered_map<std::uint16_t, std::string> tileIdMap;

    for (const auto& record : manifest.records) {
        if (record.category != "Tiles") {
            continue;
        }

        constexpr std::string_view prefix = "Tiles:";
        if (!record.id.starts_with(prefix)) {
            continue;
        }

        const auto raw = record.id.substr(prefix.size());
        std::uint16_t parsed = 0;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
        if (result.ec == std::errc {}) {
            tileIdMap[parsed] = record.id;
        }
    }

    if (tileIdMap.empty() || tileLayer.width == 0U || tileLayer.height == 0U) {
        return {};
    }

    std::vector<TileDrawEntry> entries;
    entries.reserve(static_cast<std::size_t>(tileLayer.width) * static_cast<std::size_t>(tileLayer.height));

    for (std::uint32_t y = 0; y < tileLayer.height; ++y) {
        for (std::uint32_t x = 0; x < tileLayer.width; ++x) {
            const auto flatIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(tileLayer.width) + static_cast<std::size_t>(x);
            if (flatIndex >= tileLayer.tileIndices.size()) {
                continue;
            }

            const auto sourceTileIndex = tileLayer.tileIndices[flatIndex];
            if (sourceTileIndex == 0U) {
                continue;
            }

            const auto it = tileIdMap.find(sourceTileIndex);
            if (it == tileIdMap.end()) {
                continue;
            }

            TileDrawEntry entry;
            entry.tileAssetId = it->second;
            entry.tileX = x;
            entry.tileY = y;
            entry.worldX = static_cast<float>(x) * tileSize;
            entry.worldY = static_cast<float>(y) * tileSize;
            entry.tileSize = tileSize;
            entries.push_back(entry);
        }
    }

    return entries;
}

} // namespace opm::client::render
