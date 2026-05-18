#include "screens/login_screen.hpp"
#include "client_app.hpp"
#include "imgui.h"

namespace opm::client {

LoginScreen::LoginScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{
}

ScreenTransition LoginScreen::tick(ScreenContext& ctx, double deltaSeconds)
{
    (void)ctx;
    (void)deltaSeconds;
    // No state updates needed each tick for login screen
    return {};
}

void LoginScreen::renderUI(ScreenContext& ctx)
{
    (void)ctx;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##LoginScreen", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // Center the login form
    const float windowWidth = ImGui::GetWindowWidth();
    const float windowHeight = ImGui::GetWindowHeight();
    const float formWidth = 400.0F;
    const float formHeight = 250.0F;

    ImGui::SetCursorPos(ImVec2((windowWidth - formWidth) * 0.5F, (windowHeight - formHeight) * 0.5F));

    ImGui::BeginChild("##LoginForm", ImVec2(formWidth, formHeight), true);

    ImGui::Spacing();
    ImGui::Text("Open Platformer Maker");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Username input - use fixed buffer and store in username_
    static char usernameBuffer[128] {};
    ImGui::Text("Username:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##Username", usernameBuffer, sizeof(usernameBuffer));
    username_ = usernameBuffer;

    // Password input - use fixed buffer and store in password_
    static char passwordBuffer[128] {};
    ImGui::Text("Password:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##Password", passwordBuffer, sizeof(passwordBuffer), ImGuiInputTextFlags_Password);
    password_ = passwordBuffer;

    ImGui::Spacing();

    // Error message
    if (!errorMessage_.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.0F, 0.0F, 1.0F), "Error: %s", errorMessage_.c_str());
        ImGui::Spacing();
    }

    // Login button
    if (ImGui::Button("Login", ImVec2(100, 30))) {
        if (!username_.empty() && !password_.empty()) {
            isLoggingIn_ = true;
            errorMessage_ = callbacks_.onLogin(username_, password_);
            if (errorMessage_.empty()) {
                // Login successful
                isLoggingIn_ = false;
                // Clear password for security
                password_.clear();
            } else {
                isLoggingIn_ = false;
            }
        } else {
            errorMessage_ = "Please enter username and password";
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(100, 30))) {
        callbacks_.onQuit();
    }

    // Quick test credentials hint (debug only)
    ImGui::Spacing();
    ImGui::TextDisabled("Demo: super / super");

    ImGui::EndChild();

    ImGui::End();
}

} // namespace opm::client
