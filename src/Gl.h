#pragma once

// The few OpenGL functions the app calls itself (ImGui's backend loads its own).
// They're looked up at runtime through GLFW, so no OpenGL headers or libraries are needed to build.

#include <GLFW/glfw3.h> // GLFW_INCLUDE_NONE is set in CMakeLists.txt

#include <type_traits>

namespace gl {

constexpr unsigned TEXTURE_2D = 0x0DE1;
constexpr unsigned TEXTURE_MIN_FILTER = 0x2801;
constexpr unsigned TEXTURE_MAG_FILTER = 0x2800;
constexpr unsigned TEXTURE_WRAP_S = 0x2802;
constexpr unsigned TEXTURE_WRAP_T = 0x2803;
constexpr int LINEAR = 0x2601;
constexpr int REPEAT = 0x2901;
constexpr int CLAMP_TO_EDGE = 0x812F;
constexpr unsigned RGBA = 0x1908;
constexpr unsigned UNSIGNED_BYTE = 0x1401;
constexpr unsigned COLOR_BUFFER_BIT = 0x4000;

inline void (*GenTextures)(int n, unsigned* textures);
inline void (*DeleteTextures)(int n, const unsigned* textures);
inline void (*BindTexture)(unsigned target, unsigned texture);
inline void (*TexParameteri)(unsigned target, unsigned name, int value);
inline void (*TexImage2D)(unsigned target, int level, int internalFormat, int width, int height,
                          int border, unsigned format, unsigned type, const void* pixels);
inline void (*TexSubImage2D)(unsigned target, int level, int x, int y, int width, int height,
                             unsigned format, unsigned type, const void* pixels);
inline void (*Viewport)(int x, int y, int width, int height);
inline void (*ClearColor)(float r, float g, float b, float a);
inline void (*Clear)(unsigned mask);

// Call once an OpenGL context is current
inline bool load() {
    auto get = [](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(glfwGetProcAddress(name));
        return fn != nullptr;
    };
    return get(GenTextures, "glGenTextures") && get(DeleteTextures, "glDeleteTextures") &&
           get(BindTexture, "glBindTexture") && get(TexParameteri, "glTexParameteri") &&
           get(TexImage2D, "glTexImage2D") && get(TexSubImage2D, "glTexSubImage2D") &&
           get(Viewport, "glViewport") && get(ClearColor, "glClearColor") && get(Clear, "glClear");
}

} // namespace gl
