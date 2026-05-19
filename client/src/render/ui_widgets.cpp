#include "render/ui_widgets.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI

#include "render/ui_anim.hpp"
#include "render/theme.hpp"

#include <imgui.h>
#include <cmath>
#include <cstring>

namespace opm::client::render {

// Font sizes matching the two sizes loaded in render_context.cpp.
static constexpr float kBodyFontSize  =  8.0F;
static constexpr float kTitleFontSize = 16.0F;
// PI constant (IM_PI removed in newer ImGui versions).
static constexpr float kPI = 3.14159265358979323846F;

// ---------------------------------------------------------------------------
// Internal font storage — set by RenderContext during startup.
// ---------------------------------------------------------------------------
static ImFont* s_fontBody  = nullptr;
static ImFont* s_fontTitle = nullptr;

ImFont* opmFontBody()  { return s_fontBody; }
ImFont* opmFontTitle() { return s_fontTitle; }

// Called from render_context.cpp after font atlas is built.
void opmSetFonts(ImFont* body, ImFont* title)
{
    s_fontBody  = body;
    s_fontTitle = title;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Draw layered drop-shadow behind a rect (call BEFORE drawing the content
// rect so it appears underneath).
void drawShadow(ImDrawList* dl, ImVec2 tl, ImVec2 br, float rounding)
{
    const float offsets[3] = { 2.0F, 4.0F, 7.0F };
    const ImU32 alphas[3]  = {
        OpmColor::Shadow,
        OpmColor::ShadowFar,
        OpmColor::ShadowFaint,
    };
    for (int i = 0; i < 3; ++i) {
        const float o = offsets[i];
        dl->AddRectFilled(
            ImVec2(tl.x + o, tl.y + o),
            ImVec2(br.x + o, br.y + o),
            alphas[i], rounding + 1.0F);
    }
}

// Draw outer glow — several expanding filled rects at low alpha.
void drawGlow(ImDrawList* dl, ImVec2 tl, ImVec2 br,
              ImU32 glowColor, float rounding, float intensity /* [0,1] */)
{
    if (intensity < 0.01F) return;
    // Extract color channels, override alpha for each ring.
    const ImU32 baseRgb = glowColor & 0x00FFFFFF;
    for (int ring = 4; ring >= 1; --ring) {
        const float expand  = static_cast<float>(ring) * 3.0F * intensity;
        // Outer rings are more transparent; inner rings brighter.
        const auto  alpha   = static_cast<ImU32>(
            0x2D * intensity * (1.0F - static_cast<float>(ring - 1) * 0.18F));
        dl->AddRectFilled(
            ImVec2(tl.x - expand, tl.y - expand),
            ImVec2(br.x + expand, br.y + expand),
            baseRgb | (alpha << 24),
            rounding + expand);
    }
}

// Returns the appropriate fill/hover/glow colors for a button variant.
struct ButtonColors {
    ImU32 fill;
    ImU32 fillHover;
    ImU32 glow;
    ImVec4 textColor;
};
ButtonColors resolveButtonColors(OpmButtonVariant variant)
{
    switch (variant) {
        case OpmButtonVariant::Success:
            return { OpmColor::Success, OpmColor::SuccessHover, OpmColor::GlowSuccess, OpmColor::TextV4 };
        case OpmButtonVariant::Danger:
            return { OpmColor::Danger, OpmColor::DangerHover, OpmColor::GlowDanger, OpmColor::TextV4 };
        case OpmButtonVariant::Warning:
            return { OpmColor::Warning, OpmColor::WarningHover, OpmColor::GlowWarning,
                     ImVec4(0.12F, 0.14F, 0.18F, 1.0F) };   // dark text on amber
        case OpmButtonVariant::Ghost:
            return { OpmColor::col32(0x3A, 0x42, 0x58, 0xAA),
                     OpmColor::col32(0x4A, 0x54, 0x6E, 0xCC),
                     OpmColor::GlowPrimary,
                     OpmColor::TextDimV4 };
        default: // Primary
            return { OpmColor::Primary, OpmColor::PrimaryHover, OpmColor::GlowPrimary, OpmColor::TextV4 };
    }
}

} // namespace

// ---------------------------------------------------------------------------
// OpmButton
// ---------------------------------------------------------------------------
bool OpmButton(const char* label, ImVec2 size, OpmButtonVariant variant)
{
    ImFont* font = s_fontBody;   // may be nullptr → ImGui uses default

    // Calculate label size with the chosen font.
    if (font) ImGui::PushFont(font, kBodyFontSize);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    if (font) ImGui::PopFont();

    const float padX = 16.0F;
    const float padY = 9.0F;
    ImVec2 nomSize = size;
    if (nomSize.x <= 0.0F) nomSize.x = labelSize.x + padX * 2.0F;
    if (nomSize.y <= 0.0F) nomSize.y = labelSize.y + padY * 2.0F;

    // Hit area (invisible button drives IsItemHovered / IsItemClicked).
    const ImGuiID id  = ImGui::GetID(label);
    const bool clicked = ImGui::InvisibleButton(label, nomSize);
    const bool hovered = ImGui::IsItemHovered();

    // Advance animation.
    const float hoverT    = uiAnimHoverT(id, hovered);
    const float clickProg = uiAnimClickBounce(id, clicked);
    // Scale: grows slightly on hover, pops on click.
    const float scale = 1.0F
        + 0.03F * hoverT
        + 0.05F * std::sin(clickProg * kPI);

    const ButtonColors cols = resolveButtonColors(variant);

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetItemRectMin();   // actual TL after layout
    const float  r   = ImGui::GetStyle().FrameRounding;

    // Compute scaled draw rect around item center.
    const ImVec2 center(pos.x + nomSize.x * 0.5F, pos.y + nomSize.y * 0.5F);
    const ImVec2 halfSz(nomSize.x * scale * 0.5F, nomSize.y * scale * 0.5F);
    const ImVec2 dtl(center.x - halfSz.x, center.y - halfSz.y);
    const ImVec2 dbr(center.x + halfSz.x, center.y + halfSz.y);

    // 1. Shadow
    drawShadow(dl, dtl, dbr, r);

    // 2. Outer glow (scales with hover progress)
    drawGlow(dl, dtl, dbr, cols.glow, r, hoverT);

    // 3. Fill — lerp between normal and hover color.
    const ImU32 fill = blendU32(cols.fill, cols.fillHover, hoverT);
    dl->AddRectFilled(dtl, dbr, fill, r);

    // 4. Subtle top-edge highlight (gives a slight 3D lift).
    dl->AddRectFilled(dtl, ImVec2(dbr.x, dtl.y + 3.0F),
        OpmColor::col32(0xFF, 0xFF, 0xFF, 28), r);

    // 5. Border — white at low alpha.
    dl->AddRect(dtl, dbr, OpmColor::col32(0xFF, 0xFF, 0xFF, 40), r, 0, 1.5F);

    // 6. Label text.
    if (font) ImGui::PushFont(font, kBodyFontSize);
    const ImVec2 textPos(
        center.x - labelSize.x * 0.5F,
        center.y - labelSize.y * 0.5F);
    dl->AddText(font, 0.0F,
                textPos, OpmColor::Text,
                label, nullptr);
    if (font) ImGui::PopFont();

    return clicked;
}

// ---------------------------------------------------------------------------
// OpmIconButton
// ---------------------------------------------------------------------------
bool OpmIconButton(ImTextureID icon, const char* tooltip, ImVec2 size, bool selected)
{
    const ImGuiID id = ImGui::GetID(tooltip);
    const bool clicked = ImGui::InvisibleButton(tooltip, size);
    const bool hovered = ImGui::IsItemHovered();

    const float hoverT    = uiAnimHoverT(id, hovered || selected);
    const float clickProg = uiAnimClickBounce(id, clicked);
    const float scale = 1.0F
        + 0.04F * hoverT
        + 0.06F * std::sin(clickProg * kPI);

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetItemRectMin();
    const float  r   = ImGui::GetStyle().FrameRounding;

    const ImVec2 center(pos.x + size.x * 0.5F, pos.y + size.y * 0.5F);
    const ImVec2 halfSz(size.x * scale * 0.5F, size.y * scale * 0.5F);
    const ImVec2 dtl(center.x - halfSz.x, center.y - halfSz.y);
    const ImVec2 dbr(center.x + halfSz.x, center.y + halfSz.y);

    drawShadow(dl, dtl, dbr, r);
    drawGlow(dl, dtl, dbr,
             selected ? OpmColor::GlowWarning : OpmColor::GlowPrimary,
             r, hoverT);

    // Background.
    const ImU32 bg = selected
        ? blendU32(OpmColor::col32(0x3A, 0x42, 0x58, 0xEE),
                   OpmColor::Gold, 0.35F)
        : blendU32(OpmColor::col32(0x2D, 0x33, 0x44, 0xEE),
                   OpmColor::col32(0x4A, 0x54, 0x6E, 0xEE), hoverT);
    dl->AddRectFilled(dtl, dbr, bg, r);

    if (selected) {
        // Gold border for selected state.
        dl->AddRect(dtl, dbr, OpmColor::Gold, r, 0, 2.0F);
    } else {
        dl->AddRect(dtl, dbr, OpmColor::col32(0xFF, 0xFF, 0xFF, 35), r, 0, 1.0F);
    }

    if (icon != ImTextureID_Invalid) {
        // Draw texture filling the icon area with a small inset.
        const float inset = 6.0F;
        dl->AddImage(ImTextureRef(icon),
            ImVec2(dtl.x + inset, dtl.y + inset),
            ImVec2(dbr.x - inset, dbr.y - inset));
    } else {
        // Fallback: "?" placeholder text.
        const ImVec2 tp(center.x - 4.0F, center.y - ImGui::GetTextLineHeight() * 0.5F);
        dl->AddText(nullptr, 0.0F, tp, OpmColor::TextDim, "?");
    }

    if (hovered && tooltip && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }

    return clicked;
}

// ---------------------------------------------------------------------------
// OpmBeginPanel / OpmEndPanel
// ---------------------------------------------------------------------------
void OpmBeginPanel(const char* id, ImVec2 size)
{
    // Compute where the child window will be placed so we can pre-draw
    // the shadow behind it.
    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    const ImVec2 avail     = ImGui::GetContentRegionAvail();
    ImVec2 drawSize = size;
    if (drawSize.x <= 0.0F) drawSize.x = avail.x;
    if (drawSize.y <= 0.0F) drawSize.y = avail.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r  = ImGui::GetStyle().WindowRounding;
    drawShadow(dl,
        cursorPos,
        ImVec2(cursorPos.x + drawSize.x, cursorPos.y + drawSize.y),
        r);

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
        ImVec4(0x26 / 255.0F, 0x2C / 255.0F, 0x3A / 255.0F, 0.94F));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, r);
    ImGui::BeginChild(id, size, false);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void OpmEndPanel()
{
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// OpmBeginModal / OpmEndModal
// ---------------------------------------------------------------------------
bool OpmBeginModal(const char* title, float width)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F,
               vp->WorkPos.y + vp->WorkSize.y * 0.5F),
        ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));   // auto-height

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoScrollbar |
        (title[0] == '\0' ? ImGuiWindowFlags_NoTitleBar : 0);

    // Window background with slightly more opacity for the modal feel.
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImVec4(0x1E / 255.0F, 0x24 / 255.0F, 0x34 / 255.0F, 0.97F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0F, 18.0F));

    const bool open = ImGui::Begin(title, nullptr, flags);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Draw drop shadow around this window using the background draw list
    // (renders behind the window itself).
    if (open) {
        const ImVec2 wPos = ImGui::GetWindowPos();
        const ImVec2 wSz  = ImGui::GetWindowSize();
        const float  r    = ImGui::GetStyle().WindowRounding;
        ImDrawList*  bg   = ImGui::GetBackgroundDrawList();
        drawShadow(bg, wPos, ImVec2(wPos.x + wSz.x, wPos.y + wSz.y), r);
    }

    return open;
}

void OpmEndModal()
{
    ImGui::End();
}

// ---------------------------------------------------------------------------
// OpmSectionHeader
// ---------------------------------------------------------------------------
void OpmSectionHeader(const char* label)
{
    ImFont* font = s_fontTitle;
    if (font) ImGui::PushFont(font, kTitleFontSize);

    // Accent bar left of the label.
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const float  lh = ImGui::GetTextLineHeight();
    dl->AddRectFilled(
        ImVec2(cp.x, cp.y + 1.0F),
        ImVec2(cp.x + 3.0F, cp.y + lh - 1.0F),
        OpmColor::Primary);
    ImGui::SetCursorScreenPos(ImVec2(cp.x + 8.0F, cp.y));

    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(0xA0 / 255.0F, 0xA8 / 255.0F, 0xC0 / 255.0F, 1.0F));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    if (font) ImGui::PopFont();

    // Full-width separator in accent color.
    const ImVec2 sep0(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x,
                      ImGui::GetCursorScreenPos().y);
    const ImVec2 sep1(sep0.x + ImGui::GetContentRegionAvail().x, sep0.y);
    dl->AddLine(sep0, sep1, OpmColor::col32(0x4F, 0x8D, 0xFF, 0x66), 1.0F);
    ImGui::SetCursorScreenPos(ImVec2(sep0.x - ImGui::GetStyle().WindowPadding.x + 0,
                                     sep0.y + 5.0F));
    // Re-apply correct cursor X via Spacing.
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// OpmStatusDot
// ---------------------------------------------------------------------------
void OpmStatusDot(ImVec4 color, float radius)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur  = ImGui::GetCursorScreenPos();
    const float  lineH = ImGui::GetTextLineHeight();
    const ImVec2 center(cur.x + radius, cur.y + lineH * 0.5F);
    dl->AddCircleFilled(center, radius,
        ImGui::ColorConvertFloat4ToU32(color), 16);
    // Tiny bright core.
    dl->AddCircleFilled(center, radius * 0.45F,
        OpmColor::col32(0xFF, 0xFF, 0xFF, 0x55), 12);
    // Advance the cursor past the dot.
    ImGui::Dummy(ImVec2(radius * 2.0F + 6.0F, lineH));
}

// ---------------------------------------------------------------------------
// OpmBadge
// ---------------------------------------------------------------------------
void OpmBadge(const char* text, ImVec4 color)
{
    const ImVec2 labelSz = ImGui::CalcTextSize(text);
    const float  padX    = 8.0F;
    const float  padY    = 3.0F;
    const float  r       = 6.0F;   // pill corner radius

    const ImVec2 tl  = ImGui::GetCursorScreenPos();
    const ImVec2 br(tl.x + labelSz.x + padX * 2.0F,
                    tl.y + labelSz.y + padY * 2.0F);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Semi-transparent background in the badge color.
    const ImVec4 bgColor(color.x, color.y, color.z, 0.25F);
    dl->AddRectFilled(tl, br, ImGui::ColorConvertFloat4ToU32(bgColor), r);
    // Border in the full badge color.
    dl->AddRect(tl, br, ImGui::ColorConvertFloat4ToU32(color), r, 0, 1.0F);
    // Text.
    dl->AddText(nullptr, 0.0F,
        ImVec2(tl.x + padX, tl.y + padY),
        ImGui::ColorConvertFloat4ToU32(color),
        text);

    ImGui::Dummy(ImVec2(br.x - tl.x, br.y - tl.y));
}

// ---------------------------------------------------------------------------
// OpmListItem
// ---------------------------------------------------------------------------
bool OpmListItem(const char* id, const char* detail, bool selected)
{
    const float itemHeight = ImGui::GetFrameHeight() + 2.0F;

    // Background highlight for selected row.
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Header,
            ImVec4(0xFF / 255.0F, 0xB3 / 255.0F, 0x47 / 255.0F, 0.22F));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
            ImVec4(0xFF / 255.0F, 0xB3 / 255.0F, 0x47 / 255.0F, 0.35F));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Header,
            ImVec4(0x4F / 255.0F, 0x8D / 255.0F, 0xFF / 255.0F, 0.15F));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
            ImVec4(0x4F / 255.0F, 0x8D / 255.0F, 0xFF / 255.0F, 0.25F));
    }

    // Add padding around selectable to prevent text from touching edges
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0F, 4.0F));
    const bool clicked = ImGui::Selectable(id, selected,
        ImGuiSelectableFlags_None, ImVec2(0.0F, itemHeight));
    ImGui::PopStyleVar();

    ImGui::PopStyleColor(2);

    // Selected: gold left-edge bar.
    if (selected) {
        const ImVec2 rectMin = ImGui::GetItemRectMin();
        const ImVec2 rectMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            rectMin,
            ImVec2(rectMin.x + 3.0F, rectMax.y),
            OpmColor::Gold);
    }

    // Right-aligned detail text (drawn after selectable so it overlaps).
    if (detail != nullptr) {
        const ImVec2 rectMin = ImGui::GetItemRectMin();
        const ImVec2 rectMax = ImGui::GetItemRectMax();
        const ImVec2 detailSz = ImGui::CalcTextSize(detail);
        const float detailX = rectMax.x - detailSz.x - 8.0F;
        const float detailY = rectMin.y + (itemHeight - detailSz.y) * 0.5F;
        ImGui::GetWindowDrawList()->AddText(nullptr, 0.0F,
            ImVec2(detailX, detailY),
            OpmColor::TextDim, detail);
    }

    return clicked;
}

} // namespace opm::client::render

#endif // OPM_CLIENT_HAS_IMGUI
