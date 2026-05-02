#pragma once

#ifdef OPM_CLIENT_HAS_IMGUI

#include <cstdint>

namespace opm::client::render {

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
