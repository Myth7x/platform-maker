#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

namespace opm::client {

// Active gameplay — either offline (local Simulation step) or online
// (StateUpdate polled from the server). Renders the level, players,
// actors, and the P-speed HUD.
//
// Carve-out is partial: only the Test Play HUD ("Back to Editor"
// button shown when fromEditor) lives here. The gameplay tick + render
// + input branches still live inline in runWindow because they're
// woven through the fixed-step / animation / camera-tracking machinery
// that the orchestrator owns.
class PlayingScreen final : public Screen {
public:
    explicit PlayingScreen(opm::client::game::GameSession& session);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Playing; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void render(ScreenContext& ctx) override;
    void renderUI(ScreenContext& ctx) override;

private:
    opm::client::game::GameSession* session_;
};

} // namespace opm::client
