#include "screens/online_level_select_screen.hpp"

#include "game/actor_manager.hpp"
#include "game/network_session.hpp"
#include "net_client.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

#include <cstddef>
#include <cstdio>

namespace opm::client {

OnlineLevelSelectScreen::OnlineLevelSelectScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{}

ScreenTransition OnlineLevelSelectScreen::tick(ScreenContext&, double)
{
    // Keep the connection's recv buffer drained while the user picks.
    // Without this the server's snapshot/ping/state traffic would pile
    // up and stall once gameplay starts.
    if (callbacks_.onPollServer) {
        callbacks_.onPollServer();
    }
    return {};
}

void OnlineLevelSelectScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
        ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    // Width fixed for predictable layout; height grows to fit so the
    // theme's roomy padding doesn't clip the buttons / status text.
    ImGui::SetNextWindowSize(ImVec2(520.0F, 0.0F));
    ImGui::Begin("Lobby - Choose Level", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);

    ImGui::Text("Connected as player %d. Pick a level for this lobby:",
        callbacks_.getLocalPlayerIndex ? callbacks_.getLocalPlayerIndex() : -1);
    ImGui::Separator();

    ImGui::BeginChild("##online_levels", ImVec2(0.0F, 240.0F), true);
    for (int i = 0; i < static_cast<int>(session.onlineLevels.size()); ++i) {
        const bool selected = session.onlineLevelSelected == i;
        if (ImGui::Selectable(session.onlineLevels[i].c_str(), selected)) {
            session.onlineLevelSelected = i;
        }
    }
    if (session.onlineLevels.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("(no levels on server — use the editor first, or play with the default)");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::Separator();
    const bool canSet = session.onlineLevelSelected >= 0 &&
        session.onlineLevelSelected < static_cast<int>(session.onlineLevels.size()) &&
        ctx.session != nullptr && ctx.session->isConnected();
    ImGui::BeginDisabled(!canSet);
    if (ImGui::Button("Use Selected Level", ImVec2(180.0F, 32.0F))) {
        const auto& name = session.onlineLevels[static_cast<std::size_t>(session.onlineLevelSelected)];
        const auto err = callbacks_.onUseSelectedLevel(name);
        if (err.empty()) {
            session.onlineLevelStatus.clear();
        } else {
            session.onlineLevelStatus = err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Use Current Level", ImVec2(160.0F, 32.0F))) {
        callbacks_.onUseCurrentLevel();
        session.onlineLevelStatus.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(90.0F, 32.0F))) {
        const auto err = callbacks_.onRefresh();
        if (err.empty()) {
            session.onlineLevelStatus.clear();
            session.onlineLevelSelected = -1;
        } else {
            session.onlineLevelStatus = err;
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Disconnect", ImVec2(120.0F, 28.0F))) {
        callbacks_.onDisconnect();
        session.onlineLevelStatus.clear();
    }

    if (!session.onlineLevelStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.onlineLevelStatus.c_str());
    }

    // ===== Player list footer =====
    // Pulled from ctx.net->actors so we get both the local player and
    // every remote roster entry the server has told us about. Local
    // entry is highlighted via the accent color.
    if (ctx.net != nullptr) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const auto& actors = ctx.net->actors.actors();
        std::size_t connectedCount = 0;
        for (const auto& a : actors) {
            if (a.isLocal || a.info.connected) {
                connectedCount += 1;
            }
        }
        ImGui::TextDisabled("Players in lobby (%zu):", connectedCount);
        ImGui::BeginChild("##player_list", ImVec2(0.0F, 140.0F), true);
        for (const auto& a : actors) {
            if (!a.isLocal && !a.info.connected) {
                continue;
            }
            const auto& info = a.info;
            char nameBuf[96];
            const char* displayName = !info.displayName.empty()
                ? info.displayName.c_str()
                : "Player";
            const std::uint16_t slot = a.isLocal && a.serverIndex != opm::client::game::kInvalidServerIndex
                ? a.serverIndex
                : info.playerIndex;
            std::snprintf(nameBuf, sizeof(nameBuf), "#%u  %s%s",
                static_cast<unsigned>(slot),
                displayName,
                a.isLocal ? "  (you)" : "");

            // Color swatch + name. Use the accent for local, the
            // server-provided color for remotes.
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            const ImVec4 swatch = a.isLocal
                ? accent
                : ImVec4(static_cast<float>(info.colorR) / 255.0F,
                         static_cast<float>(info.colorG) / 255.0F,
                         static_cast<float>(info.colorB) / 255.0F, 1.0F);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 cur = ImGui::GetCursorScreenPos();
            const float dotR = ImGui::GetTextLineHeight() * 0.45F;
            const ImVec2 dotCenter(cur.x + dotR, cur.y + ImGui::GetTextLineHeight() * 0.5F);
            dl->AddCircleFilled(dotCenter, dotR, ImGui::ColorConvertFloat4ToU32(swatch));
            ImGui::Dummy(ImVec2(dotR * 2.0F + 6.0F, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            if (a.isLocal) {
                ImGui::TextColored(accent, "%s", nameBuf);
            } else {
                ImGui::TextUnformatted(nameBuf);
            }
        }
        if (connectedCount == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("(no players reported yet — waiting for roster)");
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }

    ImGui::End();
#endif
}

} // namespace opm::client
