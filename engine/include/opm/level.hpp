#pragma once

#include <cstdint>
#include <vector>

namespace opm::engine {

struct TileLayer {
    std::uint32_t width {0};
    std::uint32_t height {0};
    std::vector<std::uint16_t> tileIndices {};
};

struct LevelData {
    TileLayer groundLayer {};
    float spawnX {0.0F};
    float spawnY {0.0F};
    float goalX {0.0F};
    float goalY {0.0F};
};

[[nodiscard]] LevelData createBasicLevel(std::uint32_t width, std::uint32_t height);

} // namespace opm::engine
