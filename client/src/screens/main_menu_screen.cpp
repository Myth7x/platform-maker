#include "screens/main_menu_screen.hpp"

namespace opm::client {

// TODO(refactor step 4 cont.): move the body of `if (session.state ==
// AppState::MainMenu)` from client_app.cpp's runClientWindow() into
// these methods. ScreenContext gives access to RenderContext, AssetRegistry,
// SessionClient. App-shared state (address input, GameSession) needs a
// home — likely a member here plus a shared GameSession service on
// ClientApp.
ScreenTransition MainMenuScreen::tick(ScreenContext&, double)
{
    return {};
}

void MainMenuScreen::renderUI(ScreenContext&)
{
}

} // namespace opm::client
