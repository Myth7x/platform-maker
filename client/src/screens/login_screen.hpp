#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace opm::client {

// Login screen: username + password input, login button.
// Authenticates with the server before allowing access to lobbies/menus.
class LoginScreen final : public Screen {
public:
    struct Callbacks {
        // Attempt to log in with username and password.
        // Returns error message (empty on success).
        std::function<std::string(const std::string& username, const std::string& password)>
            onLogin;

        // User clicked "Quit" button.
        std::function<void()> onQuit;
    };

    LoginScreen(opm::client::game::GameSession& session, Callbacks callbacks);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Login; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;

private:
    opm::client::game::GameSession* session_;
    Callbacks callbacks_;

    // UI state
    std::string username_ {};
    std::string password_ {};
    std::string errorMessage_ {};
    bool isLoggingIn_ {false};
};

} // namespace opm::client
