#pragma once

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
#ifdef OPM_CLIENT_WITH_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#endif

#include <GL/gl.h>

namespace opm::client::render {

struct Texture2D {
    GLuint handle {0};
    int width {0};
    int height {0};
};

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
