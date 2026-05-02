#include "screens/playing_screen.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

namespace opm::client {

PlayingScreen::PlayingScreen(opm::client::game::GameSession& session)
    : session_(&session)
{}

ScreenTransition PlayingScreen::tick(ScreenContext&, double)
{
    return {};
}

void PlayingScreen::render(ScreenContext&)
{
}

void PlayingScreen::renderUI(ScreenContext&)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;
    if (!session.fromEditor) {
        return;
    }
    // Test Play HUD: Back to Editor button.
    ImGui::SetNextWindowPos(ImVec2(8.0F, 8.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(0.0F, 0.0F));
    ImGui::Begin("##testplay_hud", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(1.0F, 0.85F, 0.3F, 1.0F), "TEST PLAY");
    ImGui::SameLine();
    if (ImGui::Button("Back to Editor")) {
        session.fromEditor = false;
        session.state = opm::client::game::AppState::LevelCreator;
    }
    ImGui::End();
#endif
}

} // namespace opm::client
