#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <functional>
#include <string>

namespace opm::client {

// Reached after joining a multiplayer lobby; lets the user pick which
// of the server's stored levels the lobby should run, or use whichever
// level the lobby is already running.
//
// While this screen is active we still need to drain incoming server
// traffic (pings, snapshots, roster updates) so the recv buffer doesn't
// fill while the user picks. That happens via the onPollServer
// callback, called each tick.
class OnlineLevelSelectScreen final : public Screen {
public:
    struct Callbacks {
        // Drain server traffic for this frame: poll any state updates,
        // drain LevelSnapshot / RosterUpdate queues, send a ping if due.
        std::function<void()> onPollServer;

        // Server's idea of who we are, for the header text.
        std::function<int()> getLocalPlayerIndex;

        // Try to set the lobby level; on success blocks briefly to
        // receive the fresh LevelSnapshot and transitions to Playing.
        // Returns error message (empty on success).
        std::function<std::string(const std::string& levelName)> onUseSelectedLevel;

        // Skip the level pick — use whichever level the lobby is
        // already running. Implementation transitions to Playing.
        std::function<void()> onUseCurrentLevel;

        // Refresh the level catalogue from the server. Returns error
        // message (empty on success).
        std::function<std::string()> onRefresh;

        // Disconnect from the server and return to MainMenu.
        std::function<void()> onDisconnect;
    };

    OnlineLevelSelectScreen(opm::client::game::GameSession& session, Callbacks callbacks);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::OnlineLevelSelect; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;

private:
    opm::client::game::GameSession* session_;
    Callbacks callbacks_;
};

} // namespace opm::client
