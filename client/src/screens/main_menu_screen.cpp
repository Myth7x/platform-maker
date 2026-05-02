#include "screens/main_menu_screen.hpp"

#include "game/network_session.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

namespace opm::client {

MainMenuScreen::MainMenuScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{}

ScreenTransition MainMenuScreen::tick(ScreenContext&, double)
{
    return {};
}

void MainMenuScreen::renderUI(ScreenContext&)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
        ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(460.0F, 0.0F));
    ImGui::Begin("Open Platformer Maker", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    // Server address — required for both Lobby Browser and Level Creator,
    // since both connect to the server (level catalogue lives there too).
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Server");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##address", session.addressInput, sizeof(session.addressInput));

    ImGui::Spacing();

    const auto resolveHostPort = [&](std::string& host, std::uint16_t& port) -> bool {
        if (!opm::client::game::parseAddress(session.addressInput, host, port)) {
            session.menuStatus = "Invalid address. Use host:port (e.g. 127.0.0.1:34900).";
            return false;
        }
        return true;
    };

    if (ImGui::Button("Lobby Browser", ImVec2(-1.0F, 40.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (resolveHostPort(host, port)) {
            const auto err = callbacks_.onOpenLobbyBrowser(host, port);
            session.menuStatus = err;
        }
    }

    if (ImGui::Button("Level Creator", ImVec2(-1.0F, 40.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (resolveHostPort(host, port)) {
            const auto err = callbacks_.onOpenLevelCreator(host, port);
            session.menuStatus = err;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Quit", ImVec2(-1.0F, 32.0F))) {
        callbacks_.onQuit();
    }

    if (!session.menuStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.menuStatus.c_str());
    }
    ImGui::End();
#endif
}

} // namespace opm::client
