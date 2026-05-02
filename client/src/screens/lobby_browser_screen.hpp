#pragma once

#include "game/game_session.hpp"
#include "screens/screen.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace opm::client {

// Browses lobbies advertised by the server. User picks one and clicks
// Join — on success the screen transitions to OnlineLevelSelect (the
// in-lobby map vote screen).
//
// Replaces the old "Play Online" auto-join flow that always picked the
// first lobby.
class LobbyBrowserScreen final : public Screen {
public:
    struct Callbacks {
        // Refresh the lobby listing from the server (connect if needed,
        // then requestLobbyList). Mutates session.availableLobbies +
        // session.lobbyBrowserStatus. Returns error message (empty on
        // success).
        std::function<std::string(const std::string& host, std::uint16_t port)>
            onRefresh;

        // Join the named lobby. On success the implementation should
        // transition session.state to OnlineLevelSelect after the join
        // completes. Returns error message (empty on success).
        std::function<std::string(const std::string& host, std::uint16_t port,
                                  const std::string& lobbyName)>
            onJoin;
    };

    LobbyBrowserScreen(opm::client::game::GameSession& session, Callbacks callbacks);

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::LobbyBrowser; }
    ScreenTransition tick(ScreenContext& ctx, double deltaSeconds) override;
    void renderUI(ScreenContext& ctx) override;

private:
    opm::client::game::GameSession* session_;
    Callbacks callbacks_;
};

} // namespace opm::client
