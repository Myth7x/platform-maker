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

    // Server address is hardcoded to the default; field hidden from menu.

    const auto resolveHostPort = [&](std::string& host, std::uint16_t& port) -> bool {
        if (!opm::client::game::parseAddress(session.addressInput, host, port)) {
            session.menuStatus = "Invalid address. Use host:port (e.g. 127.0.0.1:34900).";
            return false;
        }
        return true;
    };

    if (ImGui::Button("Play Offline (browse server levels)", ImVec2(-1.0F, 36.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (resolveHostPort(host, port)) {
            session.menuStatus = callbacks_.onEnterLevelPicker(host, port,
                opm::client::game::GameSession::PickerIntent::PlayOffline);
        }
    }

    if (ImGui::Button("Quick Offline (built-in level)", ImVec2(-1.0F, 28.0F))) {
        callbacks_.onPlayQuickOffline();
        session.menuStatus.clear();
    }

    if (ImGui::Button("Play Online", ImVec2(-1.0F, 36.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (resolveHostPort(host, port)) {
            const auto err = callbacks_.onPlayOnline(host, port);
            if (err.empty()) {
                session.menuStatus.clear();
            } else {
                session.menuStatus = err;
            }
        }
    }

    if (ImGui::Button("Create Level", ImVec2(-1.0F, 36.0F))) {
        callbacks_.onEnterLevelCreator();
        session.menuStatus.clear();
    }
    if (ImGui::Button("Edit Server Level", ImVec2(-1.0F, 36.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (resolveHostPort(host, port)) {
            session.menuStatus = callbacks_.onEnterLevelPicker(host, port,
                opm::client::game::GameSession::PickerIntent::EditOnServer);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Quit", ImVec2(-1.0F, 28.0F))) {
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
