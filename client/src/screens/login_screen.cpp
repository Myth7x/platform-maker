#include "screens/login_screen.hpp"
#include "client_app.hpp"
#include "imgui.h"

#include <cstring>

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
    const float formWidth = 320.0F;
    const float formHeight = 280.0F;

    ImGui::SetCursorPos(ImVec2((windowWidth - formWidth) * 0.5F, (windowHeight - formHeight) * 0.5F));

    // Modern style: subtle background with no border
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12F, 0.15F, 0.20F, 0.8F));
    ImGui::BeginChild("##LoginForm", ImVec2(formWidth, formHeight), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    // Padding around form content
    ImGui::Indent(16.0F);

    // Server address input
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0F);
    ImGui::Text("Server:");
    ImGui::SetNextItemWidth(-32.0F);
    ImGui::InputText("##Address", addressInput_, sizeof(addressInput_));

    // Username input
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0F);
    ImGui::Text("Username:");
    ImGui::SetNextItemWidth(-32.0F);
    static char usernameBuffer[128] {};
    ImGui::InputText("##Username", usernameBuffer, sizeof(usernameBuffer));
    username_ = usernameBuffer;

    // Password input
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0F);
    ImGui::Text("Password:");
    ImGui::SetNextItemWidth(-32.0F);
    static char passwordBuffer[128] {};
    ImGui::InputText("##Password", passwordBuffer, sizeof(passwordBuffer), ImGuiInputTextFlags_Password);
    password_ = passwordBuffer;

    // Error message
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0F);
    if (!errorMessage_.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s", errorMessage_.c_str());
    }

    // Buttons
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0F);
    const float buttonWidth = (formWidth - 50.0F) * 0.5F;
    if (ImGui::Button("Login", ImVec2(buttonWidth, 32.0F))) {
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
    if (ImGui::Button("Quit", ImVec2(buttonWidth, 32.0F))) {
        callbacks_.onQuit();
    }

    // Demo hint
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0F);
    ImGui::TextDisabled("Demo: super / super");

    ImGui::Unindent(16.0F);

    ImGui::EndChild();

    ImGui::End();
}

const char* LoginScreen::getAddressInput() const
{
    return addressInput_;
}

void LoginScreen::setAddressInput(const std::string& addr)
{
    if (addr.size() < sizeof(addressInput_)) {
        std::strncpy(addressInput_, addr.c_str(), sizeof(addressInput_) - 1);
        addressInput_[sizeof(addressInput_) - 1] = '\0';
    }
}

} // namespace opm::client
