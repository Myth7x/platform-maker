#include "render/theme.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI

#include <imgui.h>

namespace opm::client::render {
namespace {

// Convert sRGB byte components (0-255) + alpha (0..1) into ImVec4. The
// pure ImGui way of writing #RRGGBB literals.
constexpr ImVec4 rgba(int r, int g, int b, float a = 1.0F)
{
    return ImVec4(static_cast<float>(r) / 255.0F,
                  static_cast<float>(g) / 255.0F,
                  static_cast<float>(b) / 255.0F,
                  a);
}

void applyMarioMaker2()
{
    ImGuiStyle& s = ImGui::GetStyle();

    // ---- Sizes / shape ----
    // Generous rounded corners + roomy padding give the cheerful,
    // friendly feel without compromising density.
    s.WindowRounding     = 12.0F;
    s.ChildRounding      = 10.0F;
    s.FrameRounding      = 8.0F;
    s.PopupRounding      = 10.0F;
    s.GrabRounding       = 4.0F;
    s.ScrollbarRounding  = 6.0F;
    s.TabRounding        = 8.0F;
    // Borders replaced by drop shadows drawn via ImDrawList; keeping
    // borders at 0 avoids a visible seam between panels and the GL canvas.
    s.WindowBorderSize   = 0.0F;
    s.ChildBorderSize    = 0.0F;
    s.FrameBorderSize    = 0.0F;
    s.TabBorderSize      = 0.0F;
    s.WindowPadding      = ImVec2(14.0F, 12.0F);
    s.FramePadding       = ImVec2(10.0F, 6.0F);
    s.ItemSpacing        = ImVec2(8.0F, 8.0F);
    s.ItemInnerSpacing   = ImVec2(6.0F, 6.0F);
    s.IndentSpacing      = 18.0F;
    s.ScrollbarSize      = 14.0F;
    s.GrabMinSize        = 14.0F;
    s.WindowTitleAlign   = ImVec2(0.5F, 0.5F);
    s.ButtonTextAlign    = ImVec2(0.5F, 0.5F);

    // ---- Colors ----
    // Warm-dark indigo base, vivid sky-blue primary, amber highlight.
    // Window backgrounds use alpha < 1 so the GL viewport bleeds
    // through (gives the editor a "floating panels over canvas" feel).
    const ImVec4 kBgDeep      = rgba(0x1A, 0x1F, 0x2C, 0.85F);
    const ImVec4 kBgPanel     = rgba(0x26, 0x2C, 0x3A, 0.92F);
    const ImVec4 kBgFrame     = rgba(0x2D, 0x33, 0x44);
    const ImVec4 kBgFrameHov  = rgba(0x3A, 0x42, 0x58);
    const ImVec4 kBgFrameAct  = rgba(0x44, 0x4E, 0x68);
    const ImVec4 kBorder      = rgba(0x3A, 0x42, 0x58);
    const ImVec4 kText        = rgba(0xF0, 0xEB, 0xE0);
    const ImVec4 kTextDim     = rgba(0x8A, 0x8E, 0x9C);
    const ImVec4 kAccent      = rgba(0x4F, 0x8D, 0xFF);   // primary blue
    const ImVec4 kAccentHov   = rgba(0x6F, 0xA1, 0xFF);
    const ImVec4 kAccentAct   = rgba(0x34, 0x70, 0xDA);
    const ImVec4 kAmber       = rgba(0xFF, 0xB3, 0x47);   // highlight
    const ImVec4 kAmberHov    = rgba(0xFF, 0xC4, 0x70);
    const ImVec4 kAmberAct    = rgba(0xE6, 0x97, 0x2B);
    const ImVec4 kScrollGrab  = rgba(0x52, 0x5B, 0x72);
    const ImVec4 kScrollHov   = rgba(0x6A, 0x76, 0x92);

    auto& c = s.Colors;
    c[ImGuiCol_Text]                  = kText;
    c[ImGuiCol_TextDisabled]          = kTextDim;
    c[ImGuiCol_WindowBg]              = kBgDeep;
    c[ImGuiCol_ChildBg]               = kBgPanel;
    c[ImGuiCol_PopupBg]               = kBgPanel;
    c[ImGuiCol_Border]                = kBorder;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = kBgFrame;
    c[ImGuiCol_FrameBgHovered]        = kBgFrameHov;
    c[ImGuiCol_FrameBgActive]         = kBgFrameAct;

    c[ImGuiCol_TitleBg]               = kBgPanel;
    c[ImGuiCol_TitleBgActive]         = kBgPanel;
    c[ImGuiCol_TitleBgCollapsed]      = kBgPanel;
    c[ImGuiCol_MenuBarBg]             = kBgPanel;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = kScrollGrab;
    c[ImGuiCol_ScrollbarGrabHovered]  = kScrollHov;
    c[ImGuiCol_ScrollbarGrabActive]   = kAccent;

    c[ImGuiCol_CheckMark]             = kAccent;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccentHov;

    c[ImGuiCol_Button]                = kAccent;
    c[ImGuiCol_ButtonHovered]         = kAccentHov;
    c[ImGuiCol_ButtonActive]          = kAccentAct;

    c[ImGuiCol_Header]                = kAccent;
    c[ImGuiCol_HeaderHovered]         = kAccentHov;
    c[ImGuiCol_HeaderActive]          = kAccentAct;

    c[ImGuiCol_Separator]             = kBorder;
    c[ImGuiCol_SeparatorHovered]      = kAccentHov;
    c[ImGuiCol_SeparatorActive]       = kAccent;

    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = kAccentHov;
    c[ImGuiCol_ResizeGripActive]      = kAccent;

    c[ImGuiCol_Tab]                   = kBgPanel;
    c[ImGuiCol_TabHovered]            = kAccentHov;
    c[ImGuiCol_TabActive]             = kAccent;
    c[ImGuiCol_TabUnfocused]          = kBgPanel;
    c[ImGuiCol_TabUnfocusedActive]    = kBgFrame;

    c[ImGuiCol_PlotLines]             = kAmber;
    c[ImGuiCol_PlotLinesHovered]      = kAmberHov;
    c[ImGuiCol_PlotHistogram]         = kAmber;
    c[ImGuiCol_PlotHistogramHovered]  = kAmberHov;

    c[ImGuiCol_TextSelectedBg]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.40F);
    c[ImGuiCol_DragDropTarget]        = kAmber;
    c[ImGuiCol_NavHighlight]          = kAccent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.70F);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.50F);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55F);

    // Suppress unused warnings on theme constants we may not always wire.
    (void)kAmberAct;
    (void)kAmberHov;
}

} // namespace

void applyTheme(Theme theme)
{
    switch (theme) {
        case Theme::MarioMaker2:
            applyMarioMaker2();
            break;
        case Theme::ImGuiDark:
            ImGui::StyleColorsDark();
            break;
        case Theme::ImGuiLight:
            ImGui::StyleColorsLight();
            break;
    }
}

} // namespace opm::client::render

#endif // OPM_CLIENT_HAS_IMGUI
