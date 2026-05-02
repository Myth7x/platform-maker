#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include "opm/level.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace opm::client::net { class SessionClient; }

namespace opm::client {

// Browses levels stored on the server. Lets the user pick one to play
// or to load into the editor (intent comes from GameSession::pickerIntent
// — set by whichever menu button got us here).
class LevelPickerScreen final : public Screen {
public:
    struct Callbacks {
        // Called after a successful Load when the picker intent is
        // EditOnServer. The screen will not have touched gNetwork beyond
        // the load itself.
        std::function<void(opm::engine::LevelData loaded, const std::string& name)>
            onEditLoadedLevel;

        // Called after a successful Load when the picker intent is
        // PlayOffline. Implementation should clear the connection +
        // local player index and call enterPlaying(false, loaded).
        std::function<void(opm::engine::LevelData loaded)>
            onPlayLoadedLevelOffline;
    };

    LevelPickerScreen(opm::client::game::GameSession& session, Callbacks callbacks);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::LevelPicker; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;

private:
    opm::client::game::GameSession* session_;
    Callbacks callbacks_;
};

} // namespace opm::client
