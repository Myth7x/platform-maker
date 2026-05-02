#include "screens/level_picker_screen.hpp"

#include "net_client.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

#include <cstddef>

namespace opm::client {

LevelPickerScreen::LevelPickerScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{}

ScreenTransition LevelPickerScreen::tick(ScreenContext&, double)
{
    return {};
}

void LevelPickerScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
        ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(520.0F, 0.0F));
    ImGui::Begin("Level Studio", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);

    if (ImGui::Button("Create New Level", ImVec2(-1.0F, 36.0F))) {
        callbacks_.onCreateNewLevel();
    }
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Server levels (%zu):", session.serverLevels.size());
    ImGui::BeginChild("levels", ImVec2(0.0F, 220.0F), true);
    for (int i = 0; i < static_cast<int>(session.serverLevels.size()); ++i) {
        const bool selected = session.selectedLevelIndex == i;
        if (ImGui::Selectable(session.serverLevels[i].c_str(), selected)) {
            session.selectedLevelIndex = i;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            session.selectedLevelIndex = i;
        }
    }
    if (session.serverLevels.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("(no levels yet — click Create New Level above!)");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::Separator();
    const bool canLoad = session.selectedLevelIndex >= 0 &&
        session.selectedLevelIndex < static_cast<int>(session.serverLevels.size());
    ImGui::BeginDisabled(!canLoad);
    if (ImGui::Button("Load & Edit", ImVec2(220.0F, 32.0F))) {
        const auto& name = session.serverLevels[static_cast<std::size_t>(session.selectedLevelIndex)];
        opm::engine::LevelData loaded;
        std::string status;
        if (ctx.session != nullptr && ctx.session->requestLoadLevel(name, 2000U, loaded, status)) {
            callbacks_.onEditLoadedLevel(std::move(loaded), name);
            session.pickerStatus.clear();
        } else {
            session.pickerStatus = "load failed: " + status;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(120.0F, 32.0F))) {
        std::string status;
        if (ctx.session == nullptr ||
            !ctx.session->requestLevelList(2000U, session.serverLevels, status)) {
            session.pickerStatus = "refresh failed: " + status;
        } else {
            session.pickerStatus.clear();
            session.selectedLevelIndex = -1;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Back", ImVec2(80.0F, 32.0F))) {
        session.state = opm::client::game::AppState::MainMenu;
        session.pickerStatus.clear();
    }

    if (!session.pickerStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.pickerStatus.c_str());
    }
    ImGui::End();
#endif
}

} // namespace opm::client
