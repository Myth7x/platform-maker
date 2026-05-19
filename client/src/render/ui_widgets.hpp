#pragma once

#ifdef OPM_CLIENT_HAS_IMGUI

#include "render/theme.hpp"
#include <imgui.h>

namespace opm::client::render {

// ---------------------------------------------------------------------------
// OPM Widget Library
//
// Thin, animated wrapper widgets built on top of raw ImGui. Every widget uses
// ImGui::InvisibleButton (or equivalent) for hit-testing and then draws its
// own visuals via ImDrawList — giving full control over scale, glow, shadows,
// and animation that stock ImGui widgets cannot provide.
//
// Animation is frame-rate–independent (driven by ImGui::GetIO().DeltaTime).
// State is stored in a global map keyed by ImGuiID (see ui_anim.hpp).
//
// Usage pattern:
//   if (OpmButton("Play")) { ... }
//   OpmBeginPanel("##myPanel", {400, 300}); ... OpmEndPanel();
// ---------------------------------------------------------------------------

// ---- Button variants -------------------------------------------------------

enum class OpmButtonVariant {
    Primary,    // sky-blue  — default action
    Success,    // green     — confirm / play / ok
    Danger,     // red       — destructive / logout / quit
    Warning,    // amber     — caution
    Ghost,      // dim/hollow — secondary, less prominent
};

// Animated button.
//   label   — displayed text and ImGui ID source (use ##suffix for invisible ID)
//   size    — {0,0} = auto-sized from label + padding
//   variant — color scheme
//   Returns true on the frame the button is clicked.
bool OpmButton(const char* label,
               ImVec2 size    = ImVec2(0, 0),
               OpmButtonVariant variant = OpmButtonVariant::Primary);

// Convenience wrappers.
inline bool OpmButtonSuccess(const char* label, ImVec2 size = ImVec2(0, 0))
    { return OpmButton(label, size, OpmButtonVariant::Success); }
inline bool OpmButtonDanger(const char* label, ImVec2 size = ImVec2(0, 0))
    { return OpmButton(label, size, OpmButtonVariant::Danger); }
inline bool OpmButtonGhost(const char* label, ImVec2 size = ImVec2(0, 0))
    { return OpmButton(label, size, OpmButtonVariant::Ghost); }

// Small square icon button drawn with a texture. Falls back to a text label
// when icon == nullptr (useful before textures load).
// Returns true on click.
bool OpmIconButton(ImTextureID icon, const char* tooltip,
                   ImVec2 size = ImVec2(36, 36), bool selected = false);

// ---- Panels / Windows ------------------------------------------------------

// Begin a styled child-window panel with a drop shadow.
//   id    — child window ID string
//   size  — {0,0} = fill available width, auto height (prefer explicit sizes)
// Must be paired with OpmEndPanel(). Style push/pops are handled internally.
void OpmBeginPanel(const char* id, ImVec2 size = ImVec2(0, 0));
void OpmEndPanel();

// Centered floating "modal-style" window with drop shadow. Not a true ImGui
// modal (no dimming overlay) — just a prominently centered window.
//   title — window title bar text (pass "" to hide title bar)
//   width — fixed width; height is auto
// Returns true when the window is open (always true after Begin).
// Must be paired with OpmEndModal().
bool OpmBeginModal(const char* title, float width = 420.0F);
void OpmEndModal();

// ---- Layout helpers --------------------------------------------------------

// Pixel-font section header: uppercase label + full-width colored separator.
void OpmSectionHeader(const char* label);

// Inline filled dot (status indicator). Advances the cursor so a SameLine()
// widget sits next to it at text baseline.
void OpmStatusDot(ImVec4 color, float radius = 5.0F);

// Rounded pill badge — short text in a filled rounded rectangle.
// Typically used for capacity counts, vote tallies, error tags.
void OpmBadge(const char* text, ImVec4 color);

// Styled list row. Wraps ImGui::Selectable with gold highlight for selection,
// an optional right-aligned detail string, and hover highlight.
//   id       — ImGui ID (label + "##id" or just the display label)
//   detail   — right-aligned secondary text; pass nullptr to omit
//   selected — whether this row is currently selected
// Returns true when clicked.
bool OpmListItem(const char* id, const char* detail, bool selected);

// ---- Font accessors --------------------------------------------------------
// Fonts are loaded in RenderContext. These return nullptr before init, in
// which case ImGui falls back to its default built-in font gracefully.
ImFont* opmFontBody();   // Press Start 2P 8px  (body text, buttons)
ImFont* opmFontTitle();  // Press Start 2P 16px (window titles, headers)

// Called once by RenderContext after ImGui font atlas is built.
void opmSetFonts(ImFont* body, ImFont* title);

} // namespace opm::client::render

#endif // OPM_CLIENT_HAS_IMGUI
