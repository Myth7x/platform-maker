#pragma once

#include "screens/screen.hpp"

namespace opm::client {

// Active gameplay — either offline (local Simulation step) or online
// (StateUpdate polled from the server). Renders the level, players,
// actors, and the P-speed HUD.
class PlayingScreen final : public Screen {
public:
    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Playing; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void render(ScreenContext& ctx) override;
    void renderUI(ScreenContext& ctx) override;
};

} // namespace opm::client
