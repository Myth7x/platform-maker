#include "screens/main_menu_screen.hpp"

#include "game/network_session.hpp"

#include <cstdio>

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

void MainMenuScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;
    auto* netSession = ctx.session;

    const char* kIconLabels[] = {"A", "B", "C", "D", "E"};
    constexpr std::size_t kIconCount = sizeof(kIconLabels) / sizeof(kIconLabels[0]);
    const std::uint8_t safeIconId = static_cast<std::uint8_t>(session.profileIconId % kIconCount);

    const auto resolveHostPort = [&](std::string& host, std::uint16_t& port) -> bool {
        if (!opm::client::game::parseAddress(session.addressInput, host, port)) {
            session.menuStatus = "Invalid address. Use host:port (e.g. 127.0.0.1:34900).";
            return false;
        }
        return true;
    };

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MainMenuRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    // Top navbar
    ImGui::BeginChild("TopNavbar", ImVec2(0.0F, 56.0F), true);
    const bool connected = (netSession != nullptr && netSession->isConnected());
    const ImVec4 statusColor = connected
        ? ImVec4(0.35F, 0.92F, 0.55F, 1.0F)
        : ImVec4(1.0F, 0.42F, 0.42F, 1.0F);
    ImGui::TextColored(statusColor, "%s", connected ? "Connected" : "Disconnected");
    if (connected) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%ums)", netSession->getPingMs());
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("Server");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputText("##address", session.addressInput, sizeof(session.addressInput));

    const float rightAnchor = ImGui::GetWindowWidth() - 160.0F;
    if (rightAnchor > ImGui::GetCursorPosX()) {
        ImGui::SameLine(rightAnchor);
    }

    const std::string displayName = session.displayName.empty() ? session.username : session.displayName;
    ImGui::TextDisabled("%s", displayName.empty() ? "Guest" : displayName.c_str());
    ImGui::SameLine();
    if (ImGui::Button(kIconLabels[safeIconId], ImVec2(34.0F, 28.0F))) {
        ImGui::OpenPopup("ProfileMenu");
    }
    if (ImGui::BeginPopup("ProfileMenu")) {
        if (ImGui::MenuItem("Edit Profile")) {
            profileEditorOpen_ = true;
            profileEditorName_ = displayName;
            profileEditorIconId_ = safeIconId;
        }
        ImGui::Separator();
        ImGui::TextDisabled("@%s", session.username.empty() ? "unknown" : session.username.c_str());
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Center content card
    const float cardWidth = 520.0F;
    const float cardX = (vp->WorkSize.x - cardWidth) * 0.5F;
    if (cardX > 0.0F) {
        ImGui::SetCursorPosX(cardX);
    }
    ImGui::BeginChild("MainMenuCard", ImVec2(cardWidth, 0.0F), true);
    ImGui::TextUnformatted("Open Platformer Maker");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Lobby Browser", ImVec2(-1.0F, 44.0F))) {
        std::string host;
        std::uint16_t port = 0;
        if (resolveHostPort(host, port)) {
            const auto err = callbacks_.onOpenLobbyBrowser(host, port);
            session.menuStatus = err;
        }
    }

    if (ImGui::Button("Level Creator", ImVec2(-1.0F, 44.0F))) {
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

    ImGui::EndChild();

    if (profileEditorOpen_) {
        ImGui::OpenPopup("Edit Profile");
        profileEditorOpen_ = false;
    }

    if (ImGui::BeginPopupModal("Edit Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Display name");
        char nameBuffer[64] {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", profileEditorName_.c_str());
        if (ImGui::InputText("##displayName", nameBuffer, sizeof(nameBuffer))) {
            profileEditorName_ = nameBuffer;
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Profile icon");
        for (std::size_t i = 0; i < kIconCount; ++i) {
            if (i != 0) {
                ImGui::SameLine();
            }
            const bool selected = (profileEditorIconId_ == static_cast<std::uint8_t>(i));
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30F, 0.54F, 0.96F, 1.0F));
            }
            if (ImGui::Button(kIconLabels[i], ImVec2(34.0F, 28.0F))) {
                profileEditorIconId_ = static_cast<std::uint8_t>(i);
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(120.0F, 0.0F))) {
            std::string host;
            std::uint16_t port = 0;
            if (!resolveHostPort(host, port)) {
                // resolveHostPort sets a user-friendly status already.
            } else if (profileEditorName_.empty()) {
                session.menuStatus = "Display name cannot be empty.";
            } else {
                const auto err = callbacks_.onUpdateProfile(host, port, profileEditorName_);
                if (err.empty()) {
                    session.displayName = profileEditorName_;
                    session.profileIconId = profileEditorIconId_;
                    session.menuStatus = "Profile updated.";
                    ImGui::CloseCurrentPopup();
                } else {
                    session.menuStatus = err;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0F, 0.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
#endif
}

} // namespace opm::client
