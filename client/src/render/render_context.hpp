#pragma once

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)

struct GLFWwindow;

namespace opm::client::render {

// RAII wrapper around the windowing/graphics stack: glfwInit + window
// creation + (when built with the OpenGL stub) GL state setup +
// (when built with ImGui) ImGui context + GLFW/OpenGL2 backends.
//
// Construct once at the top of the client run loop. Check ok() before
// using; on failure, the destructor is still safe to run. The destructor
// tears everything down in reverse order.
class RenderContext {
public:
    RenderContext(int width, int height, const char* title);
    ~RenderContext();

    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] GLFWwindow* window() const noexcept { return window_; }

private:
    GLFWwindow* window_ {nullptr};
    bool glfwInitialized_ {false};
    bool imguiInitialized_ {false};
    bool ok_ {false};
};

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB || OPM_CLIENT_WITH_VULKAN
