#include "screens/login_screen.hpp"
#include "client_app.hpp"
#include "imgui.h"
#include "render/ui_widgets.hpp"

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
    using namespace opm::client::render;

    // Full-screen background window (no decoration).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##LoginBg", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();

    // Centered floating modal card.
    if (!OpmBeginModal("##LoginModal", 340.0F)) {
        OpmEndModal();
        return;
    }

    // Title.
    {
        ImFont* titleFont = opmFontTitle();
        if (titleFont) ImGui::PushFont(titleFont);
        const float titleW = ImGui::CalcTextSize("OPM").x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - titleW) * 0.5F);
        ImGui::TextColored(OpmColor::PrimaryV4, "OPM");
        if (titleFont) ImGui::PopFont();
    }
    ImGui::Spacing();

    // Server address.
    ImGui::TextDisabled("Server");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##Address", addressInput_, sizeof(addressInput_));
    ImGui::Spacing();

    // Username.
    ImGui::TextDisabled("Username");
    ImGui::SetNextItemWidth(-1.0F);
    static char usernameBuffer[128] {};
    ImGui::InputText("##Username", usernameBuffer, sizeof(usernameBuffer));
    username_ = usernameBuffer;
    ImGui::Spacing();

    // Password.
    ImGui::TextDisabled("Password");
    ImGui::SetNextItemWidth(-1.0F);
    static char passwordBuffer[128] {};
    ImGui::InputText("##Password", passwordBuffer, sizeof(passwordBuffer),
        ImGuiInputTextFlags_Password);
    password_ = passwordBuffer;
    ImGui::Spacing();

    // Error badge.
    if (!errorMessage_.empty()) {
        OpmBadge(errorMessage_.c_str(), OpmColor::DangerV4);
        ImGui::Spacing();
    }

    // Action buttons side by side.
    const float btnW = (ImGui::GetContentRegionAvail().x - 8.0F) * 0.5F;
    if (OpmButton("Login", ImVec2(btnW, 38.0F))) {
        if (!username_.empty() && !password_.empty()) {
            isLoggingIn_ = true;
            errorMessage_ = callbacks_.onLogin(username_, password_);
            isLoggingIn_ = false;
            if (errorMessage_.empty()) {
                password_.clear();
                std::memset(passwordBuffer, 0, sizeof(passwordBuffer));
            }
        } else {
            errorMessage_ = "Please enter username and password";
        }
    }
    ImGui::SameLine(0.0F, 8.0F);
    if (OpmButtonGhost("Quit", ImVec2(btnW, 38.0F))) {
        callbacks_.onQuit();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Demo: super / super");

    OpmEndModal();
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
