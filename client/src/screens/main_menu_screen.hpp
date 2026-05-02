#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace opm::client {

// Top-level menu: offline / online / create / edit / quit.
//
// Owns no session-wide state of its own — reads `addressInput` and
// `menuStatus` from the GameSession passed in at construction. Action
// buttons fan out through a Callbacks struct populated by ClientApp;
// this keeps the screen decoupled from runWindow's local lambdas while
// the rest of the orchestrator is being carved up.
class MainMenuScreen final : public Screen {
public:
    struct Callbacks {
        // Connect to host:port and open the level picker. Returns error
        // message (empty on success). Caller threads the picker intent
        // through (PlayOffline vs EditOnServer).
        std::function<std::string(const std::string& host,
                                  std::uint16_t port,
                                  opm::client::game::GameSession::PickerIntent)>
            onEnterLevelPicker;

        // Quick-play the built-in fallback level offline.
        std::function<void()> onPlayQuickOffline;

        // Run the multiplayer lobby join flow + level-list fetch + state
        // transition to OnlineLevelSelect. Returns error message (empty
        // on success).
        std::function<std::string(const std::string& host, std::uint16_t port)>
            onPlayOnline;

        // Enter the level creator with a blank canvas.
        std::function<void()> onEnterLevelCreator;

        // Quit the app (glfwSetWindowShouldClose).
        std::function<void()> onQuit;
    };

    MainMenuScreen(opm::client::game::GameSession& session, Callbacks callbacks);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::MainMenu; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;

private:
    opm::client::game::GameSession* session_;
    Callbacks callbacks_;
};

} // namespace opm::client
