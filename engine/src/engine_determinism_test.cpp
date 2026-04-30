#include "opm/engine.hpp"

#include <array>
#include <cassert>

int main()
{
    opm::engine::Simulation simA;
    opm::engine::Simulation simB;

    for (std::uint32_t frame = 0; frame < 600; ++frame) {
        std::array<opm::engine::InputFrame, 2> inputs {};
        const bool p0JumpHeld = (frame % 90U) < 8U;
        const bool p1JumpHeld = (frame % 150U) < 10U;
        inputs[0] = opm::engine::InputFrame {
            frame,
            (frame % 120U) < 20U,
            (frame % 120U) > 60U,
            frame % 90U == 0U,
            p0JumpHeld,
            (frame % 200U) > 100U
        };
        inputs[1] = opm::engine::InputFrame {
            frame,
            (frame % 180U) > 40U,
            (frame % 240U) < 30U,
            frame % 150U == 0U,
            p1JumpHeld,
            (frame % 160U) < 40U
        };

        simA.step(inputs);
        simB.step(inputs);
    }

    assert(simA.stateHash() == simB.stateHash());
    return 0;
}
