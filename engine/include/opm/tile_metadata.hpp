#pragma once

#include <cstdint>

namespace opm::engine {

struct TileCollisionMask {
    bool solidTop {false};
    bool solidBottom {false};
    bool solidLeft {false};
    bool solidRight {false};
    // Semi-solid (one-way) platform: only blocks a downward landing when the
    // player's feet were above the tile's top edge before the step. Players
    // can jump up through it and may drop through it by holding crouch.
    bool oneWayTop {false};
};

[[nodiscard]] TileCollisionMask collisionMaskForTile(std::uint16_t tileIndex);

} // namespace opm::engine
