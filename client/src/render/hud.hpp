#pragma once

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

namespace opm::client::render {

// Magenta translucent quad with a yellow outline at the player's
// collision box. Diagnostic only; called from the gameplay render
// branch when debug overlay is enabled.
void drawDebugPlayerBody(float x0, float y0, float x1, float y1);

// Bottom-left meter showing the running player's P-speed charge.
// `meterValue` is clamped to [0,1]; `pSpeedActive` switches the fill
// from red (charging) to gold (active).
void drawPSpeedHud(float meterValue, bool pSpeedActive,
                   int framebufferWidth, int framebufferHeight);

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
