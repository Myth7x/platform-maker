#include "screens/lobby_browser_screen.hpp"

#include "game/network_session.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#include "render/ui_widgets.hpp"
#endif

#include <cstddef>

namespace opm::client {

LobbyBrowserScreen::LobbyBrowserScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{}

ScreenTransition LobbyBrowserScreen::tick(ScreenContext&, double)
{
    return {};
}

void LobbyBrowserScreen::renderUI(ScreenContext&)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    using namespace opm::client::render;
    auto& session = *session_;

    if (!OpmBeginModal("Join Game", 540.0F)) {
        OpmEndModal();
        return;
    }

    ImGui::TextDisabled("Server: %s", session.addressInput);
    ImGui::Spacing();
    OpmSectionHeader("Available Lobbies");

    ImGui::BeginChild("##lobbies", ImVec2(0.0F, 200.0F), false);
    for (int i = 0; i < static_cast<int>(session.availableLobbies.size()); ++i) {
        const auto& lobby = session.availableLobbies[static_cast<std::size_t>(i)];
        const bool selected = session.selectedLobbyIndex == i;
        char detail[32];
        std::snprintf(detail, sizeof(detail), "%u/%u", lobby.players, lobby.capacity);
        if (OpmListItem(lobby.name.c_str(), detail, selected)) {
            session.selectedLobbyIndex = i;
        }
    }
    if (session.availableLobbies.empty()) {
        ImGui::TextDisabled("(no lobbies — click Refresh, or check the server is running)");
    }
    ImGui::EndChild();

    ImGui::Spacing();

    const bool canJoin = session.selectedLobbyIndex >= 0 &&
        session.selectedLobbyIndex < static_cast<int>(session.availableLobbies.size());
    ImGui::BeginDisabled(!canJoin);
    if (OpmButtonSuccess("Join", ImVec2(120.0F, 36.0F))) {
        const auto& name =
            session.availableLobbies[static_cast<std::size_t>(session.selectedLobbyIndex)].name;
        std::string host;
        std::uint16_t port = 0;
        if (!opm::client::game::parseAddress(session.addressInput, host, port)) {
            session.lobbyBrowserStatus = "Invalid server address.";
        } else {
            const auto err = callbacks_.onJoin(host, port, name);
            if (!err.empty()) {
                session.lobbyBrowserStatus = err;
            } else {
                session.lobbyBrowserStatus.clear();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, 8.0F);
    if (OpmButton("Refresh", ImVec2(120.0F, 36.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (!opm::client::game::parseAddress(session.addressInput, host, port)) {
            session.lobbyBrowserStatus = "Invalid server address.";
        } else {
            const auto err = callbacks_.onRefresh(host, port);
            if (!err.empty()) {
                session.lobbyBrowserStatus = err;
            } else {
                session.lobbyBrowserStatus.clear();
                session.selectedLobbyIndex = -1;
            }
        }
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (OpmButtonGhost("Back", ImVec2(80.0F, 36.0F))) {
        session.state = opm::client::game::AppState::MainMenu;
        session.lobbyBrowserStatus.clear();
    }

    if (!session.lobbyBrowserStatus.empty()) {
        ImGui::Spacing();
        OpmBadge(session.lobbyBrowserStatus.c_str(), OpmColor::DangerV4);
    }
    OpmEndModal();
#endif
}

} // namespace opm::client
