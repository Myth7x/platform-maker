#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include "opm/level.hpp"

#include <functional>
#include <string>

namespace opm::client::net { class SessionClient; }

namespace opm::client {

// "Level Studio": browse server-stored levels and either edit one or
// create a new blank level. Replaces the dual-purpose picker that used
// to also serve offline play (offline mode removed).
class LevelPickerScreen final : public Screen {
public:
    struct Callbacks {
        // Open the editor on a freshly loaded level. Implementation
        // typically calls editLoadedLevel(loaded, name).
        std::function<void(opm::engine::LevelData loaded, const std::string& name)>
            onEditLoadedLevel;

        // Open the editor with a blank canvas (Create New Level button).
        std::function<void()> onCreateNewLevel;
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
