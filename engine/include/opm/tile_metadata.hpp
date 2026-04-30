#pragma once

#include <cstdint>

namespace opm::engine {

struct TileCollisionMask {
    bool solidTop {false};
    bool solidBottom {false};
    bool solidLeft {false};
    bool solidRight {false};
};

[[nodiscard]] TileCollisionMask collisionMaskForTile(std::uint16_t tileIndex);

} // namespace opm::engine
