#pragma once

#include "screens/screen.hpp"

namespace opm::client {

// Top-level menu: offline / online / create / edit / quit. Owns its own
// transient UI fields (address input, status string).
//
// Stub: implementation lives in client_app.cpp's runClientWindow() until
// that monolith is carved up. Once carved, this class' tick() will own
// the per-frame work currently inlined under `if (session.state ==
// AppState::MainMenu)`.
class MainMenuScreen final : public Screen {
public:
    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::MainMenu; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;
};

} // namespace opm::client
