#include "opm/tile_metadata.hpp"

namespace opm::engine {

TileCollisionMask collisionMaskForTile(const std::uint16_t tileIndex)
{
    switch (tileIndex) {
    case 0U:
    case 17U: // Water decorative tile
    case 18U: // Sky flat color tile
        return {};

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
