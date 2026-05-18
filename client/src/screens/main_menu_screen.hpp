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
        // Open the lobby browser (connect to server + fetch lobby list,
        // then transition to LobbyBrowser screen). Returns error
        // message (empty on success).
        std::function<std::string(const std::string& host, std::uint16_t port)>
            onOpenLobbyBrowser;

        // Open the level studio: a list of server-stored levels with a
        // "Create New Level" button. Returns error message (empty on
        // success). Implementation transitions to the level picker.
        std::function<std::string(const std::string& host, std::uint16_t port)>
            onOpenLevelCreator;

        // Update remote profile display name on the server.
        // Returns error message (empty on success).
        std::function<std::string(const std::string& host, std::uint16_t port, const std::string& displayName)>
            onUpdateProfile;

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
    bool profileEditorOpen_ {false};
    std::string profileEditorName_ {};
    std::uint8_t profileEditorIconId_ {0};
};

} // namespace opm::client
