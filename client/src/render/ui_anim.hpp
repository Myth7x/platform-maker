#pragma once

#ifdef OPM_CLIENT_HAS_IMGUI

#include <imgui.h>
#include <unordered_map>
#include <cmath>

namespace opm::client::render {

// Per-widget animation state. Keyed by ImGuiID in getAnimStates().
struct AnimState {
    float hoverT    = 0.0F;   // [0,1] hover lerp progress
    float clickTime = -1.0F;  // ImGui::GetTime() at last click, -1 = none
};

// Global map; populated on-demand. Old entries for removed widgets are
// benign (small memory cost, no behavioral effect).
inline std::unordered_map<ImGuiID, AnimState>& getAnimStates()
{
    static std::unordered_map<ImGuiID, AnimState> s;
    return s;
}

// Advance and return hoverT for the given widget ID.
//   hovered   — true when the widget is currently hovered
//   inSpeed   — lerp speed (units/sec) when entering hover
//   outSpeed  — lerp speed when leaving hover
inline float uiAnimHoverT(ImGuiID id, bool hovered,
                           float inSpeed = 10.0F, float outSpeed = 6.0F)
{
    auto& st = getAnimStates()[id];
    const float dt     = ImGui::GetIO().DeltaTime;
    const float target = hovered ? 1.0F : 0.0F;
    const float speed  = hovered ? inSpeed : outSpeed;
    st.hoverT += (target - st.hoverT) * speed * dt;
    if (st.hoverT < 0.001F) st.hoverT = 0.0F;
    if (st.hoverT > 0.999F) st.hoverT = 1.0F;
    return st.hoverT;
}

// Record a click and return [0,1] bounce progress for the current frame.
// Apply to scale: scale = 1 + 0.06*sin(progress * PI) for a pop effect.
inline float uiAnimClickBounce(ImGuiID id, bool clicked,
                                float duration = 0.12F)
{
    auto& st = getAnimStates()[id];
    if (clicked) {
        st.clickTime = static_cast<float>(ImGui::GetTime());
    }
    if (st.clickTime < 0.0F) return 0.0F;
    const float elapsed = static_cast<float>(ImGui::GetTime()) - st.clickTime;
    if (elapsed >= duration) {
        st.clickTime = -1.0F;
        return 0.0F;
    }
    return elapsed / duration;   // [0, 1)
}

// Linear interpolation of two ImVec4 colors.
inline ImVec4 lerpColor(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

// Blend two IM_COL32 packed colors by factor t in [0,1].
inline ImU32 blendU32(ImU32 a, ImU32 b, float t)
{
    const auto ra = static_cast<float>((a >>  0) & 0xFF);
    const auto ga = static_cast<float>((a >>  8) & 0xFF);
    const auto ba_ = static_cast<float>((a >> 16) & 0xFF);
    const auto aa = static_cast<float>((a >> 24) & 0xFF);
    const auto rb = static_cast<float>((b >>  0) & 0xFF);
    const auto gb = static_cast<float>((b >>  8) & 0xFF);
    const auto bb_ = static_cast<float>((b >> 16) & 0xFF);
    const auto ab = static_cast<float>((b >> 24) & 0xFF);
    const auto ri = static_cast<ImU32>(ra + (rb - ra) * t);
    const auto gi = static_cast<ImU32>(ga + (gb - ga) * t);
    const auto bi = static_cast<ImU32>(ba_ + (bb_ - ba_) * t);
    const auto ai = static_cast<ImU32>(aa + (ab - aa) * t);
    return (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

} // namespace opm::client::render

#endif // OPM_CLIENT_HAS_IMGUI
