#include "render/hud.hpp"

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include <GL/gl.h>

#include <algorithm>

namespace opm::client::render {

void drawDebugPlayerBody(const float x0, const float y0, const float x1, const float y1)
{
    glBindTexture(GL_TEXTURE_2D, 0);
    glColor4f(1.0F, 0.0F, 1.0F, 0.35F);
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();

    glColor3f(1.0F, 0.95F, 0.2F);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

void drawPSpeedHud(const float meterValue, const bool pSpeedActive,
                   const int framebufferWidth, const int framebufferHeight)
{
    (void)framebufferWidth;
    const float clamped = std::clamp(meterValue, 0.0F, 1.0F);

    const float x = 12.0F;
    const float y = static_cast<float>(framebufferHeight) - 18.0F;
    const float w = 74.0F;
    const float h = 8.0F;

    glBindTexture(GL_TEXTURE_2D, 0);

    glColor4f(0.06F, 0.08F, 0.10F, 0.75F);
    glBegin(GL_QUADS);
    glVertex2f(x - 2.0F, y - 2.0F);
    glVertex2f(x + w + 2.0F, y - 2.0F);
    glVertex2f(x + w + 2.0F, y + h + 2.0F);
    glVertex2f(x - 2.0F, y + h + 2.0F);
    glEnd();

    const float fillW = w * clamped;
    if (fillW > 0.0F) {
        if (pSpeedActive) {
            glColor3f(1.0F, 0.86F, 0.16F);
        } else {
            glColor3f(0.80F, 0.22F, 0.22F);
        }
        glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + fillW, y);
        glVertex2f(x + fillW, y + h);
        glVertex2f(x, y + h);
        glEnd();
    }

    glColor3f(0.95F, 0.95F, 0.95F);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
