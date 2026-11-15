#include "App.h"
#include "Gl.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ipp.h>

#include <cstdio>
#include <filesystem>

// Rebuilds the font and widget sizes for a new UI scale. Must be called between
// frames, not between ImGui::NewFrame() and ImGui::Render().
void applyUiScale(float scale, const ImGuiStyle& baseStyle) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    if (std::filesystem::exists(SDR_FONT_PATH)) {
        io.Fonts->AddFontFromFileTTF(SDR_FONT_PATH, 15.0f * scale);
    } else {
        std::fprintf(stderr, "Font not found at %s, using ImGui's default\n", SDR_FONT_PATH);
        ImFontConfig font;
        font.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&font);
    }
    // The OpenGL backend re-uploads the font texture on the next NewFrame()
    ImGui_ImplOpenGL3_DestroyFontsTexture();

    ImGui::GetStyle() = baseStyle;
    ImGui::GetStyle().ScaleAllSizes(scale);
}

int main() {
    ippInit(); // select the fastest IPP code path for this CPU

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1400, 850, "SDR App", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync
    if (!gl::load()) {
        std::fprintf(stderr, "Failed to load OpenGL functions\n");
        return 1;
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // don't write imgui.ini
    ImGui::StyleColorsDark();
    const ImGuiStyle baseStyle = ImGui::GetStyle(); // unscaled, so scaling never compounds

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // High-DPI screens report a content scale above 1; the user's zoom multiplies it
    float screenScale = 1.0f;
    glfwGetWindowContentScale(window, &screenScale, nullptr);
    float appliedZoom = 0.0f;

    {
        App app; // destroyed before the OpenGL context, as it owns a texture

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (app.uiZoom() != appliedZoom) {
                appliedZoom = app.uiZoom();
                applyUiScale(screenScale * appliedZoom, baseStyle);
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            app.draw();

            ImGui::Render();
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            gl::Viewport(0, 0, width, height);
            gl::ClearColor(0.08f, 0.08f, 0.08f, 1.0f);
            gl::Clear(gl::COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
