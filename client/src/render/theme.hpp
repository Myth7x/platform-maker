#pragma once

#ifdef OPM_CLIENT_HAS_IMGUI

#include <imgui.h>
#include <cstdint>

namespace opm::client::render {

// ---------------------------------------------------------------------------
// OpmColor — named color constants for the Mario Maker 2 palette.
//
// ImU32 variants (IM_COL32 format) are for ImDrawList calls.
// Vec4 helper functions return ImVec4 for PushStyleColor calls.
// ---------------------------------------------------------------------------
namespace OpmColor {

// Helper: pack RGBA bytes into an IM_COL32-compatible ImU32.
inline constexpr ImU32 col32(unsigned r, unsigned g, unsigned b, unsigned a = 255)
{
    return (static_cast<ImU32>(a) << 24)
         | (static_cast<ImU32>(b) << 16)
         | (static_cast<ImU32>(g) <<  8)
         | (static_cast<ImU32>(r));
}

// Helper: build ImVec4 from 0-255 components + 0-1 alpha.
inline constexpr ImVec4 vec4(unsigned r, unsigned g, unsigned b, float a = 1.0F)
{
    return ImVec4(
        static_cast<float>(r) / 255.0F,
        static_cast<float>(g) / 255.0F,
        static_cast<float>(b) / 255.0F,
        a);
}

// ---- Primary (sky-blue) ----
inline constexpr ImU32  Primary      = col32(0x4F, 0x8D, 0xFF);
inline constexpr ImU32  PrimaryHover = col32(0x7F, 0xB3, 0xFF);
inline constexpr ImU32  PrimaryActive= col32(0x34, 0x70, 0xDA);
inline constexpr ImU32  GlowPrimary  = col32(0x4F, 0x8D, 0xFF, 0x2D);  // 18% alpha
inline constexpr ImVec4 PrimaryV4    = vec4(0x4F, 0x8D, 0xFF);
inline constexpr ImVec4 PrimaryHovV4 = vec4(0x7F, 0xB3, 0xFF);

// ---- Success (green) ----
inline constexpr ImU32  Success      = col32(0x4D, 0xC4, 0x72);
inline constexpr ImU32  SuccessHover = col32(0x72, 0xD4, 0x8E);
inline constexpr ImU32  SuccessActive= col32(0x36, 0xA0, 0x58);
inline constexpr ImU32  GlowSuccess  = col32(0x4D, 0xC4, 0x72, 0x2D);
inline constexpr ImVec4 SuccessV4    = vec4(0x4D, 0xC4, 0x72);
inline constexpr ImVec4 SuccessHovV4 = vec4(0x72, 0xD4, 0x8E);

// ---- Danger (red) ----
inline constexpr ImU32  Danger       = col32(0xE0, 0x52, 0x52);
inline constexpr ImU32  DangerHover  = col32(0xEA, 0x7A, 0x7A);
inline constexpr ImU32  DangerActive = col32(0xBF, 0x38, 0x38);
inline constexpr ImU32  GlowDanger   = col32(0xE0, 0x52, 0x52, 0x2D);
inline constexpr ImVec4 DangerV4     = vec4(0xE0, 0x52, 0x52);
inline constexpr ImVec4 DangerHovV4  = vec4(0xEA, 0x7A, 0x7A);

// ---- Warning / Gold (amber) ----
inline constexpr ImU32  Warning      = col32(0xFF, 0xB3, 0x47);
inline constexpr ImU32  WarningHover = col32(0xFF, 0xC8, 0x70);
inline constexpr ImU32  GlowWarning  = col32(0xFF, 0xB3, 0x47, 0x2D);
inline constexpr ImVec4 WarningV4    = vec4(0xFF, 0xB3, 0x47);
inline constexpr ImVec4 WarningHovV4 = vec4(0xFF, 0xC8, 0x70);

// Gold highlight — selected tiles, vote leaders, etc.
inline constexpr ImU32  Gold         = col32(0xFF, 0xB3, 0x47);
inline constexpr ImU32  GoldHover    = col32(0xFF, 0xC8, 0x70);
inline constexpr ImU32  GoldSelected = col32(0xFF, 0xB3, 0x47, 0x55);  // 33% alpha fill

// ---- Backgrounds ----
inline constexpr ImU32  DeepBg       = col32(0x1A, 0x1F, 0x2C, 0xD9);  // 85% alpha
inline constexpr ImU32  PanelBg      = col32(0x26, 0x2C, 0x3A, 0xEB);  // 92% alpha
inline constexpr ImU32  FrameBg      = col32(0x2D, 0x33, 0x44, 0xFF);

// ---- Text ----
inline constexpr ImU32  Text         = col32(0xF0, 0xEB, 0xE0);
inline constexpr ImU32  TextDim      = col32(0x8A, 0x8E, 0x9C);
inline constexpr ImVec4 TextV4       = vec4(0xF0, 0xEB, 0xE0);
inline constexpr ImVec4 TextDimV4    = vec4(0x8A, 0x8E, 0x9C);

// ---- Shadow / Glow ----
inline constexpr ImU32  Shadow       = col32(0x00, 0x00, 0x00, 0x88);  // 53% alpha
inline constexpr ImU32  ShadowFar    = col32(0x00, 0x00, 0x00, 0x44);  // 27% alpha
inline constexpr ImU32  ShadowFaint  = col32(0x00, 0x00, 0x00, 0x22);  // 13% alpha

} // namespace OpmColor

// Visual themes for the ImGui UI. Apply once after ImGui::CreateContext
// (RenderContext does this in its constructor). Switching themes at
// runtime is supported — call applyTheme again from anywhere.
enum class Theme : std::uint8_t {
    // Default: warm dark indigo with vivid blue/amber accents, generous
    // rounded corners, bold padding. Inspired by Mario Maker 2's
    // creator UI — cheerful but readable in an editor context.
    MarioMaker2,

    // Classic ImGui themes, kept for fallback / comparison.
    ImGuiDark,
    ImGuiLight,
};

inline constexpr Theme kDefaultTheme = Theme::MarioMaker2;

// Mutates the current ImGui style (ImGui::GetStyle()) and the per-color
// table. Safe to call repeatedly.
void applyTheme(Theme theme);

} // namespace opm::client::render

#endif // OPM_CLIENT_HAS_IMGUI
