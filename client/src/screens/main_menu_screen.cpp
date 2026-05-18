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
    auto* net = ctx.net;

    const auto resolveHostPort = [&](std::string& host, std::uint16_t& port) -> bool {
        if (!opm::client::game::parseAddress(session.addressInput, host, port)) {
            session.menuStatus = "Invalid address. Use host:port (e.g. 127.0.0.1:34900).";
            return false;
        }
        return true;
    };

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("MainMenuRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    // Top navbar - compact height with vertical centering
    const float navbarHeight = 40.0F;
    const float navbarPadding = 8.0F;
    
    const bool connected = (netSession != nullptr && netSession->isConnected());
    const ImVec4 statusColor = connected
        ? ImVec4(0.35F, 0.92F, 0.55F, 1.0F)
        : ImVec4(1.0F, 0.42F, 0.42F, 1.0F);
    
    // Draw navbar background - full width from x0 to screen edge
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float screenPosX = vp->Pos.x;
    const float screenPosY = vp->Pos.y;
    const ImVec2 navbarTopLeft(screenPosX, screenPosY);
    const ImVec2 navbarBottomRight(screenPosX + vp->Size.x, screenPosY + navbarHeight);
    drawList->AddRectFilled(navbarTopLeft, navbarBottomRight, ImGui::GetColorU32(ImGuiCol_FrameBg));
    drawList->AddLine(ImVec2(screenPosX, screenPosY + navbarHeight), navbarBottomRight, ImGui::GetColorU32(ImGuiCol_Border));
    
    // Vertically center within navbar
    const float textCenterY = screenPosY + (navbarHeight - ImGui::GetTextLineHeight()) * 0.5F;
    ImGui::SetCursorPos(ImVec2(navbarPadding, textCenterY - screenPosY));
    
    // Left side: connection status
    ImGui::TextColored(statusColor, "%s", connected ? "Connected" : "Disconnected");
    
    if (connected) {
        ImGui::SameLine(0.0F, 8.0F);
        ImGui::TextDisabled("(%ums)", netSession->getPingMs());
    }
    
    // Right side: user profile and name
    const char* kIconLabels[] = {"A", "B", "C", "D", "E"};
    constexpr std::size_t kIconCount = sizeof(kIconLabels) / sizeof(kIconLabels[0]);
    const std::uint8_t safeIconId = static_cast<std::uint8_t>(session.profileIconId % kIconCount);
    
    const std::string displayName = session.displayName.empty() ? session.username : session.displayName;
    const float profileButtonWidth = 34.0F;
    const float profileButtonHeight = 28.0F;
    const float nameWidth = ImGui::CalcTextSize(displayName.empty() ? "Guest" : displayName.c_str()).x;
    const float rightContentWidth = nameWidth + profileButtonWidth + 20.0F; // 20px spacing
    const float rightX = vp->Size.x - rightContentWidth - navbarPadding;
    
    ImGui::SetCursorPos(ImVec2(rightX, textCenterY - screenPosY));
    ImGui::TextDisabled("%s", displayName.empty() ? "Guest" : displayName.c_str());
    ImGui::SameLine(0.0F, 8.0F);
    
    const float buttonY = (navbarHeight - profileButtonHeight) * 0.5F;
    ImGui::SetCursorPos(ImVec2(rightX + nameWidth + 8.0F, buttonY));
    if (ImGui::Button(kIconLabels[safeIconId], ImVec2(profileButtonWidth, profileButtonHeight))) {
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
    
    // Move cursor below navbar
    ImGui::SetCursorPosY(navbarHeight);

    const float contentHeight = vp->Size.y - navbarHeight;
    const float contentPadding = 8.0F;
    const float leftPaneWidth = vp->Size.x * 0.7F - contentPadding;
    const float rightPaneWidth = vp->Size.x * 0.3F - contentPadding;

    // Left pane: Lobby Browser
    ImGui::SetCursorPos(ImVec2(contentPadding, navbarHeight + contentPadding));
    ImGui::BeginChild("LobbyPane", ImVec2(leftPaneWidth, contentHeight - contentPadding * 2), true);
    
    // Lobby state
    static std::vector<opm::client::game::LobbyListing> displayLobbies;
    static bool lobbiesLoaded = false;
    
    ImGui::TextUnformatted("Lobby Browser");
    ImGui::SameLine(ImGui::GetWindowWidth() - 90.0F);
    if (ImGui::Button("Refresh##lobby", ImVec2(80.0F, 0.0F))) {
        lobbiesLoaded = false;  // Force refresh on next frame
    }
    ImGui::Separator();
    
    // Fetch lobbies if needed
    // Fetch lobbies if needed
    if (!lobbiesLoaded) {
        std::string host;
        std::uint16_t port = 0;
        if (!resolveHostPort(host, port)) {
            // Error message already set in resolveHostPort
        } else if (net == nullptr) {
            session.menuStatus = "Network context not initialized";
        } else if (net->session == nullptr) {
            session.menuStatus = "Network session not connected";
        } else {
            const auto result = fetchLobbyList(*net, host, port, displayLobbies);
            lobbiesLoaded = result.ok;
            if (!result.ok) {
                session.menuStatus = result.message;
            }
        }
    }
    
    // Show lobbies
    if (displayLobbies.empty()) {
        ImGui::TextDisabled("(no lobbies available)");
    } else {
        for (const auto& lobby : displayLobbies) {
            const std::string label = lobby.name + " (" + std::to_string(lobby.players) + "/" + 
                                      std::to_string(lobby.capacity) + ")";
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    std::string host;
                    std::uint16_t port = 0;
                    if (resolveHostPort(host, port)) {
                        const auto err = callbacks_.onJoinLobby(host, port, lobby.name);
                        session.menuStatus = err;
                    }
                }
            }
        }
    }
    
    if (!session.menuStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.menuStatus.c_str());
    }
    
    ImGui::EndChild();

    // Right pane: Actions
    ImGui::SetCursorPos(ImVec2(vp->Size.x - rightPaneWidth - contentPadding, navbarHeight + contentPadding));
    ImGui::BeginChild("ActionsPane", ImVec2(rightPaneWidth, contentHeight - contentPadding * 2), true);

    // Create Lobby button
    if (ImGui::Button("Create Lobby", ImVec2(-1.0F, 44.0F))) {
        createLobbyDialogOpen_ = true;
        createLobbyName_.clear();
        createLobbyStatus_.clear();
    }

    ImGui::Spacing();

    // Level Creator button
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

    // Logout button
    if (ImGui::Button("Logout", ImVec2(-1.0F, 32.0F))) {
        callbacks_.onLogout();
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

    if (createLobbyDialogOpen_) {
        ImGui::OpenPopup("Create Lobby");
        createLobbyDialogOpen_ = false;
    }

    if (ImGui::BeginPopupModal("Create Lobby", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Lobby name");
        char nameBuffer[64] {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", createLobbyName_.c_str());
        if (ImGui::InputText("##lobbyName", nameBuffer, sizeof(nameBuffer))) {
            createLobbyName_ = nameBuffer;
        }

        ImGui::Spacing();
        if (!createLobbyStatus_.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s", createLobbyStatus_.c_str());
            ImGui::Spacing();
        }

        if (ImGui::Button("Create", ImVec2(120.0F, 0.0F))) {
            if (createLobbyName_.empty()) {
                createLobbyStatus_ = "Lobby name cannot be empty";
            } else if (netSession != nullptr && netSession->isConnected()) {
                std::string status;
                if (netSession->requestCreateLobby(createLobbyName_, 2000U, status)) {
                    session.menuStatus = "Lobby created!";
                    lobbiesLoaded = false;  // Force refresh of lobby list
                    ImGui::CloseCurrentPopup();
                } else {
                    createLobbyStatus_ = "Failed: " + status;
                }
            } else {
                createLobbyStatus_ = "Not connected to server";
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
