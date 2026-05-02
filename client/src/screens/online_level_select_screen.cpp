#include "screens/online_level_select_screen.hpp"

#include "game/actor_manager.hpp"
#include "game/network_session.hpp"
#include "net_client.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
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

#ifdef OPM_CLIENT_HAS_IMGUI
namespace {

// Small section header: dim uppercase label + thin separator.
void sectionHeader(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// Filled circle drawn inline at the current cursor; advances the cursor
// past it so the next SameLine'd widget aligns vertically with the dot.
void inlineDot(const ImVec4& color, float radius)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 center(cur.x + radius, cur.y + lineH * 0.5F);
    dl->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(color));
    ImGui::Dummy(ImVec2(radius * 2.0F + 8.0F, lineH));
}

} // namespace
#endif

void OnlineLevelSelectScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
        ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(560.0F, 0.0F));
    ImGui::Begin("Lobby", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);

    // Determine voting mode: more than one connected player → cooperative
    // map vote; otherwise single-player can pick directly.
    std::size_t connectedCount = 0;
    if (ctx.net != nullptr) {
        for (const auto& a : ctx.net->actors.actors()) {
            if (a.isLocal || a.info.connected) {
                connectedCount += 1;
            }
        }
    }
    const bool votingMode = connectedCount >= 2U;
    const int localSlot = callbacks_.getLocalPlayerIndex ? callbacks_.getLocalPlayerIndex() : -1;

    const ImVec4 accent     = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 amber      (0.95F, 0.74F, 0.30F, 1.0F);
    const ImVec4 dimText    = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

    // ===== Header =====
    ImGui::Text("You are player");
    ImGui::SameLine();
    ImGui::TextColored(accent, "#%d", localSlot);
    ImGui::SameLine();
    ImGui::TextDisabled(votingMode
        ? "  -  multiplayer mode (vote to pick the next level)"
        : "  -  single player (pick a level to start)");

    ImGui::Spacing();

    // ===== Per-level vote helpers =====
    auto countVotesFor = [&](const std::string& name) -> int {
        int n = 0;
        for (const auto& v : session.mapVoteTally) {
            if (v.levelName == name) {
                n += 1;
            }
        }
        return n;
    };
    auto isMyVote = [&](const std::string& name) -> bool {
        if (localSlot < 0) {
            return false;
        }
        for (const auto& v : session.mapVoteTally) {
            if (v.slotIndex == static_cast<std::uint16_t>(localSlot) && v.levelName == name) {
                return true;
            }
        }
        return false;
    };

    // ===== Levels =====
    sectionHeader(votingMode ? "VOTE FOR A LEVEL" : "LEVELS");

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_PadOuterX;
    const float listH = 240.0F;
    if (ImGui::BeginTable("##levels", 2, kTableFlags, ImVec2(0.0F, listH))) {
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Votes", ImGuiTableColumnFlags_WidthFixed, 90.0F);

        for (int i = 0; i < static_cast<int>(session.onlineLevels.size()); ++i) {
            const auto& name = session.onlineLevels[static_cast<std::size_t>(i)];
            const bool selected = session.onlineLevelSelected == i;
            const int votes = votingMode ? countVotesFor(name) : 0;
            const bool myVote = votingMode && isMyVote(name);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            // Selectable spans both columns so the whole row is hot.
            if (ImGui::Selectable(name.c_str(), selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                session.onlineLevelSelected = i;
                if (votingMode) {
                    callbacks_.onCastVote(name);
                }
            }
            ImGui::TableNextColumn();
            if (votingMode) {
                if (votes > 0) {
                    ImGui::TextColored(myVote ? amber : accent, "%d %s",
                        votes, votes == 1 ? "vote" : "votes");
                } else {
                    ImGui::TextDisabled("-");
                }
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (session.onlineLevels.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::TextWrapped("(no levels on server — make one in the editor first)");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // ===== Actions =====
    if (votingMode) {
        const bool canVote = session.onlineLevelSelected >= 0 &&
            session.onlineLevelSelected < static_cast<int>(session.onlineLevels.size());
        ImGui::BeginDisabled(!canVote);
        // Primary action — wider, accent-colored (theme already paints
        // it accent so just give it presence).
        if (ImGui::Button("Cast Vote", ImVec2(180.0F, 36.0F))) {
            const auto& name = session.onlineLevels[static_cast<std::size_t>(session.onlineLevelSelected)];
            callbacks_.onCastVote(name);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Withdraw", ImVec2(120.0F, 36.0F))) {
            callbacks_.onCastVote("");
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(110.0F, 36.0F))) {
            const auto err = callbacks_.onRefresh();
            if (err.empty()) {
                session.onlineLevelStatus.clear();
                session.onlineLevelSelected = -1;
            } else {
                session.onlineLevelStatus = err;
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::TextUnformatted("Most-voted level loads when the round starts.");
        ImGui::PopStyleColor();
    } else {
        const bool canSet = session.onlineLevelSelected >= 0 &&
            session.onlineLevelSelected < static_cast<int>(session.onlineLevels.size()) &&
            ctx.session != nullptr && ctx.session->isConnected();
        ImGui::BeginDisabled(!canSet);
        if (ImGui::Button("Use Selected", ImVec2(180.0F, 36.0F))) {
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
        if (ImGui::Button("Use Current", ImVec2(140.0F, 36.0F))) {
            callbacks_.onUseCurrentLevel();
            session.onlineLevelStatus.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(110.0F, 36.0F))) {
            const auto err = callbacks_.onRefresh();
            if (err.empty()) {
                session.onlineLevelStatus.clear();
                session.onlineLevelSelected = -1;
            } else {
                session.onlineLevelStatus = err;
            }
        }
    }

    if (!session.onlineLevelStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F),
                           "%s", session.onlineLevelStatus.c_str());
    }

    // ===== Players =====
    if (ctx.net != nullptr) {
        ImGui::Spacing();
        char playersHeader[64];
        std::snprintf(playersHeader, sizeof(playersHeader),
            "PLAYERS  (%zu)", connectedCount);
        sectionHeader(playersHeader);

        const float playerListH = std::max(80.0F,
            std::min(180.0F, static_cast<float>(connectedCount) * 28.0F + 20.0F));

        if (ImGui::BeginTable("##players", 2, kTableFlags, ImVec2(0.0F, playerListH))) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Vote", ImGuiTableColumnFlags_WidthFixed, 160.0F);

            for (const auto& a : ctx.net->actors.actors()) {
                if (!a.isLocal && !a.info.connected) {
                    continue;
                }
                const auto& info = a.info;
                const std::uint16_t slot = a.isLocal &&
                    a.serverIndex != opm::client::game::kInvalidServerIndex
                    ? a.serverIndex
                    : info.playerIndex;
                const char* displayName = !info.displayName.empty()
                    ? info.displayName.c_str()
                    : "Player";

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const ImVec4 swatch = a.isLocal
                    ? accent
                    : ImVec4(static_cast<float>(info.colorR) / 255.0F,
                             static_cast<float>(info.colorG) / 255.0F,
                             static_cast<float>(info.colorB) / 255.0F, 1.0F);
                inlineDot(swatch, ImGui::GetTextLineHeight() * 0.45F);
                ImGui::SameLine();
                if (a.isLocal) {
                    ImGui::TextColored(accent, "#%u  %s  (you)",
                        static_cast<unsigned>(slot), displayName);
                } else {
                    ImGui::Text("#%u  %s", static_cast<unsigned>(slot), displayName);
                }

                ImGui::TableNextColumn();
                const char* votedLevel = nullptr;
                for (const auto& v : session.mapVoteTally) {
                    if (v.slotIndex == slot && !v.levelName.empty()) {
                        votedLevel = v.levelName.c_str();
                        break;
                    }
                }
                if (votedLevel != nullptr) {
                    ImGui::TextColored(amber, "-> %s", votedLevel);
                } else {
                    ImGui::TextDisabled(votingMode ? "(no vote)" : "-");
                }
            }
            ImGui::EndTable();
        }
        if (connectedCount == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, dimText);
            ImGui::TextWrapped("(no players reported yet — waiting for roster)");
            ImGui::PopStyleColor();
        }
    }

    // ===== Footer =====
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Disconnect", ImVec2(140.0F, 32.0F))) {
        callbacks_.onDisconnect();
        session.onlineLevelStatus.clear();
    }

    ImGui::End();
#endif
}

} // namespace opm::client
