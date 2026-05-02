#include "opm/level.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace opm::engine {
namespace {

std::size_t tileOffset(const std::uint32_t x, const std::uint32_t y, const std::uint32_t width)
{
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
}

void setTile(TileLayer& layer, const std::uint32_t x, const std::uint32_t y, const std::uint16_t tile)
{
    if (x >= layer.width || y >= layer.height) {
        return;
    }
    layer.tileIndices[tileOffset(x, y, layer.width)] = tile;
}

void fillGroundColumn(TileLayer& layer, const std::uint32_t x, const std::uint32_t topY, const std::uint16_t topTile, const std::uint16_t fillTile)
{
    if (x >= layer.width || topY >= layer.height) {
        return;
    }

    setTile(layer, x, topY, topTile);
    for (std::uint32_t y = 0; y < topY; ++y) {
        setTile(layer, x, y, fillTile);
    }
}

void addPipe(TileLayer& layer, const std::uint32_t baseX, const std::uint32_t topY, const std::uint32_t height)
{
    constexpr std::uint16_t kPipeLeft = 4U;
    constexpr std::uint16_t kPipeRight = 6U;
    constexpr std::uint16_t kPipeCore = 5U;

    for (std::uint32_t step = 0; step < height; ++step) {
        const std::uint32_t y = topY - step;
        if (step == 0U) {
            setTile(layer, baseX, y, kPipeLeft);
            setTile(layer, baseX + 1U, y, kPipeRight);
        } else {
            setTile(layer, baseX, y, kPipeCore);
            setTile(layer, baseX + 1U, y, kPipeCore);
        }
    }
}

void addFlatPlatform(TileLayer& layer, const std::uint32_t fromX, const std::uint32_t toX, const std::uint32_t y)
{
    constexpr std::uint16_t kTopLeft = 1U;
    constexpr std::uint16_t kTopMid = 2U;
    constexpr std::uint16_t kTopRight = 3U;

    if (fromX > toX) {
        return;
    }

    if (fromX == toX) {
        setTile(layer, fromX, y, kTopMid);
        return;
    }

    setTile(layer, fromX, y, kTopLeft);
    for (std::uint32_t x = fromX + 1U; x < toX; ++x) {
        setTile(layer, x, y, kTopMid);
    }
    setTile(layer, toX, y, kTopRight);
}

void addStair(TileLayer& layer, const std::uint32_t startX, const std::uint32_t baseY, const std::uint32_t steps)
{
    constexpr std::uint16_t kGroundFill = 2U;
    for (std::uint32_t step = 0; step < steps; ++step) {
        for (std::uint32_t height = 0; height <= step; ++height) {
            setTile(layer, startX + step, baseY + height, kGroundFill);
        }
    }
}

} // namespace

LevelData createBasicLevel(const std::uint32_t width, const std::uint32_t height)
{
    LevelData level;
    resizeAllLayers(level, width, height);

    if (width == 0U || height == 0U) {
        return level;
    }

    constexpr std::uint16_t kGroundTopLeft = 1U;
    constexpr std::uint16_t kGroundTopMid = 2U;
    constexpr std::uint16_t kGroundTopRight = 3U;
    constexpr std::uint16_t kGroundFill = 5U;

    const std::uint32_t groundY = std::min<std::uint32_t>(2U, height - 1U);

    // SMB 1-1 inspired: long flat ground with gaps, pipes, blocks, and final stairs.
    for (std::uint32_t x = 0; x < width; ++x) {
        const bool gap =
            (x >= 58U && x <= 60U) ||
            (x >= 94U && x <= 96U) ||
            (x >= 139U && x <= 141U) ||
            (x >= 171U && x <= 173U);
        if (gap) {
            continue;
        }
        const bool leftGap = (x == 0U) ? true : (
            (x - 1U >= 58U && x - 1U <= 60U) ||
            (x - 1U >= 94U && x - 1U <= 96U) ||
            (x - 1U >= 139U && x - 1U <= 141U) ||
            (x - 1U >= 171U && x - 1U <= 173U)
        );
        const bool rightGap = (x + 1U >= width) ? true : (
            (x + 1U >= 58U && x + 1U <= 60U) ||
            (x + 1U >= 94U && x + 1U <= 96U) ||
            (x + 1U >= 139U && x + 1U <= 141U) ||
            (x + 1U >= 171U && x + 1U <= 173U)
        );

        std::uint16_t topTile = kGroundTopMid;
        if (leftGap && !rightGap) {
            topTile = kGroundTopLeft;
        } else if (!leftGap && rightGap) {
            topTile = kGroundTopRight;
        }

        fillGroundColumn(level.foliage, x, groundY, topTile, kGroundFill);
    }

    // Floating platforms with proper edge/mid tiles.
    addFlatPlatform(level.foliage, 20U, 26U, groundY + 4U);
    addFlatPlatform(level.foliage, 35U, 38U, groundY + 4U);

    // Pipes.
    addPipe(level.foliage, 45U, groundY + 3U, 3U);
    addPipe(level.foliage, 67U, groundY + 4U, 4U);
    addPipe(level.foliage, 83U, groundY + 5U, 5U);
    addPipe(level.foliage, 118U, groundY + 4U, 4U);

    // Mid-course platforming blocks.
    addFlatPlatform(level.foliage, 110U, 115U, groundY + 6U);
    addFlatPlatform(level.foliage, 126U, 129U, groundY + 5U);

    // End stairs + flag approach.
    addStair(level.foliage, 186U, groundY + 1U, 7U);
    addStair(level.foliage, 198U, groundY + 1U, 4U);

    level.spawnX = 3.0F;
    level.spawnY = static_cast<float>(groundY + 1U);
    level.goalX = static_cast<float>(width > 8U ? width - 8U : width - 1U);
    level.goalY = static_cast<float>(groundY + 8U);
    return level;
}

void resizeAllLayers(LevelData& level, std::uint32_t width, std::uint32_t height)
{
    auto resizeLayer = [&](TileLayer& layer) {
        std::vector<std::uint16_t> previous = std::move(layer.tileIndices);
        const std::uint32_t prevW = layer.width;
        const std::uint32_t prevH = layer.height;
        layer.width = width;
        layer.height = height;
        layer.tileIndices.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
        const std::uint32_t copyW = std::min(prevW, width);
        const std::uint32_t copyH = std::min(prevH, height);
        for (std::uint32_t y = 0; y < copyH; ++y) {
            for (std::uint32_t x = 0; x < copyW; ++x) {
                const auto srcIdx = static_cast<std::size_t>(y) * prevW + x;
                const auto dstIdx = static_cast<std::size_t>(y) * width + x;
                if (srcIdx < previous.size() && dstIdx < layer.tileIndices.size()) {
                    layer.tileIndices[dstIdx] = previous[srcIdx];
                }
            }
        }
    };
    resizeLayer(level.background);
    resizeLayer(level.foliage);
    resizeLayer(level.foreground);
}

SpawnSafeZone computeSpawnSafeZone(const LevelData& level) noexcept
{
    // Snap to the spawn tile center so the zone is grid-aligned regardless
    // of any sub-tile drift in spawnX/Y.
    const float cx = std::floor(level.spawnX) + 0.5F;
    const float cy = std::floor(level.spawnY) + 0.5F;
    SpawnSafeZone z;
    z.minX = cx - kSpawnSafeZoneHalfTiles;
    z.minY = cy - kSpawnSafeZoneHalfTiles;
    z.maxX = cx + kSpawnSafeZoneHalfTiles;
    z.maxY = cy + kSpawnSafeZoneHalfTiles;
    return z;
}

bool aabbOverlapsSpawnSafeZone(const LevelData& level,
    const float x, const float y, const float w, const float h) noexcept
{
    const SpawnSafeZone z = computeSpawnSafeZone(level);
    return (x + w) > z.minX && x < z.maxX
        && (y + h) > z.minY && y < z.maxY;
}

} // namespace opm::engine
