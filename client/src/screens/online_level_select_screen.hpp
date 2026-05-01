#pragma once

#include "screens/screen.hpp"

namespace opm::client {

// Reached after joining a multiplayer lobby; lets the user pick which
// of the server's stored levels the lobby should run.
class OnlineLevelSelectScreen final : public Screen {
public:
    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::OnlineLevelSelect; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;
};

} // namespace opm::client
