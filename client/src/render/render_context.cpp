#include "render/render_context.hpp"

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)

#ifdef OPM_CLIENT_WITH_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
#include <GL/gl.h>
#endif

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>
#endif

#include <iostream>

namespace opm::client::render {

RenderContext::RenderContext(int width, int height, const char* title)
{
    if (glfwInit() == GLFW_FALSE) {
        std::cerr << "[client] failed to initialize GLFW\n";
        return;
    }
    glfwInitialized_ = true;

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
#else
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif

    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (window_ == nullptr) {
        std::cerr << "[client] failed to create GLFW window\n";
        return;
    }

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
#endif

#ifdef OPM_CLIENT_HAS_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL2_Init();
    imguiInitialized_ = true;
#endif

    ok_ = true;
}

RenderContext::~RenderContext()
{
#ifdef OPM_CLIENT_HAS_IMGUI
    if (imguiInitialized_) {
        ImGui_ImplOpenGL2_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
#endif
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (glfwInitialized_) {
        glfwTerminate();
    }
}

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB || OPM_CLIENT_WITH_VULKAN
