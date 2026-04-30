#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "opm/level.hpp"

namespace opm::engine {

// SMB1 small Mario approximates about 12x16 pixels in a 16px tile grid.
inline constexpr float kPlayerWidthTiles = 0.75F;
inline constexpr float kPlayerHeightTiles = 1.0F;

struct Vec2 {
    float x {0.0F};
    float y {0.0F};
};

struct InputFrame {
    std::uint32_t frameIndex {0};
    bool moveLeft {false};
    bool moveRight {false};
    bool jumpPressed {false};
    bool jumpHeld {false};
    bool runHeld {false};
    bool crouchHeld {false};
};

struct PlayerState {
    Vec2 position {};
    Vec2 velocity {};
    bool onGround {true};
    bool facingRight {true};
    bool skidding {false};
    bool crouching {false};
    bool pSpeedActive {false};
    std::uint8_t jumpHoldFrames {0};
    std::uint8_t jumpBufferFrames {0};
    float pSpeedMeter {0.0F};
};

struct GameState {
    std::uint32_t tick {0};
    std::array<PlayerState, 2> players {};
};

class Simulation {
public:
    Simulation();

    void reset();
    void setLevel(LevelData level);
    void step(const std::array<InputFrame, 2>& inputs);

    [[nodiscard]] const GameState& state() const;
    [[nodiscard]] const LevelData& level() const;
    [[nodiscard]] std::uint64_t stateHash() const;

private:
    void integratePlayer(PlayerState& player, const InputFrame& input);

    GameState state_ {};
    LevelData level_ {};
};

[[nodiscard]] std::vector<std::uint8_t> serializeInput(const InputFrame& input);
[[nodiscard]] InputFrame deserializeInput(const std::vector<std::uint8_t>& bytes);

} // namespace opm::engine
