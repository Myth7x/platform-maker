#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <string>

namespace opm::client {

// Active gameplay — either offline (local Simulation step) or online
// (StateUpdate polled from the server). Owns the camera tracking and
// world rendering for the Playing state, plus the Test Play HUD.
class PlayingScreen final : public Screen {
public:
    explicit PlayingScreen(opm::client::game::GameSession& session);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Playing; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void render(ScreenContext& ctx) override;
    void renderUI(ScreenContext& ctx) override;

private:
    // Maps a tile asset id to a stable pseudo-random pastel color, used
    // when the asset has no texture loaded (so missing tiles still
    // appear distinguishable from each other).
    static void colorForAsset(const std::string& assetId, float& r, float& g, float& b);

    opm::client::game::GameSession* session_;
};

} // namespace opm::client
