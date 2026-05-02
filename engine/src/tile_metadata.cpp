#include "opm/tile_metadata.hpp"

#include "opm/level.hpp"

namespace opm::engine {

TileCollisionMask collisionMaskForTile(const std::uint16_t tileIndex)
{
    // Tile cells store rotation in the upper 2 bits; mask them off so the
    // lookup keys on the base tile id only. Rotation-aware collision (e.g.
    // for slopes) can be added later by also reading tileRotationSteps().
    switch (tileBaseIndex(tileIndex)) {
    case 0U:
    case 17U: // Water decorative tile
    case 18U: // Sky flat color tile
        return {};

    // Semi-solid platform: jump up through, land on top, drop through
    // when crouching. Encoded by setting only solidTop + oneWayTop.
    case 19U:
        return TileCollisionMask {
            .solidTop = true,
            .solidBottom = false,
            .solidLeft = false,
            .solidRight = false,
            .oneWayTop = true,
        };

    // Grass/dirt body tiles.
    case 1U:
    case 2U:
    case 3U:
    case 4U:
    case 5U:
    case 6U:
    case 7U:
    case 8U:
    case 9U:
    case 10U:
    case 11U:
    case 12U:
    case 13U:
    case 14U:
    case 15U:
    case 16U:
        return TileCollisionMask {true, true, true, true};

    default:
        return TileCollisionMask {true, true, true, true};
    }
}

} // namespace opm::engine
