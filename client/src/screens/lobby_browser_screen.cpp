#include "screens/lobby_browser_screen.hpp"

#include "game/network_session.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
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
    auto& session = *session_;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
        ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(520.0F, 0.0F));
    ImGui::Begin("Lobby Browser", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);

    ImGui::Text("Server: %s", session.addressInput);
    ImGui::Separator();

    ImGui::TextDisabled("Available lobbies (%zu):", session.availableLobbies.size());
    ImGui::BeginChild("##lobbies", ImVec2(0.0F, 220.0F), true);
    for (int i = 0; i < static_cast<int>(session.availableLobbies.size()); ++i) {
        const auto& lobby = session.availableLobbies[static_cast<std::size_t>(i)];
        const bool selected = session.selectedLobbyIndex == i;
        char label[256];
        std::snprintf(label, sizeof(label), "%s   (%u/%u)",
            lobby.name.c_str(), lobby.players, lobby.capacity);
        if (ImGui::Selectable(label, selected)) {
            session.selectedLobbyIndex = i;
        }
    }
    if (session.availableLobbies.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("(no lobbies — click Refresh, or check the server is running)");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::Separator();
    const bool canJoin = session.selectedLobbyIndex >= 0 &&
        session.selectedLobbyIndex < static_cast<int>(session.availableLobbies.size());
    ImGui::BeginDisabled(!canJoin);
    if (ImGui::Button("Join", ImVec2(120.0F, 32.0F))) {
        const auto& name =
            session.availableLobbies[static_cast<std::size_t>(session.selectedLobbyIndex)].name;
        // Parse address from session.addressInput.
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
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(120.0F, 32.0F))) {
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
    ImGui::SameLine();
    if (ImGui::Button("Back", ImVec2(80.0F, 32.0F))) {
        session.state = opm::client::game::AppState::MainMenu;
        session.lobbyBrowserStatus.clear();
    }

    if (!session.lobbyBrowserStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F),
                           "%s", session.lobbyBrowserStatus.c_str());
    }
    ImGui::End();
#endif
}

} // namespace opm::client
