#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <functional>
#include <string>

namespace opm::engine { struct LevelData; }

namespace opm::client {

// Active gameplay — either offline (local Simulation step) or online
// (StateUpdate polled from the server). Owns the gameplay tick
// (fixed-step input + sim + network drain), camera tracking + world
// rendering, and the Test Play HUD.
class PlayingScreen final : public Screen {
public:
    struct Callbacks {
        // Apply a fresh server-pushed level snapshot mid-session:
        // rebuild draw entries, re-setLevel the simulation, reset the
        // local spawn position. Caller already updated
        // session.activeLevel and net.networkLevel.
        std::function<void(const opm::engine::LevelData& level)> onLevelSnapshotChanged;
    };

    PlayingScreen(opm::client::game::GameSession& session, Callbacks callbacks);

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
    Callbacks callbacks_ {};

    // Per-frame gameplay timing + input edge-detect. Held on the screen
    // so menu transitions naturally reset on re-entry to Playing.
    double previousTime_ {0.0};
    double accumulator_ {0.0};
    bool   jumpHeldLast_ {false};
    float  animationTime_ {0.0F};
    bool   timingInitialized_ {false};
};

} // namespace opm::client
