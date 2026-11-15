cmake_minimum_required(VERSION 3.24)
project(sdr-app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- System dependencies ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTLSDR REQUIRED IMPORTED_TARGET librtlsdr)

find_package(glfw3 3.3 REQUIRED)  # apt: libglfw3-dev
find_package(Threads REQUIRED)

# Intel oneAPI IPP, linked statically so no oneAPI environment is needed at runtime
find_package(IPP REQUIRED COMPONENTS ipps ippvm ippcore
    HINTS /opt/intel/oneapi/ipp/latest/lib/cmake/ipp)

# ---- Fetched dependencies ----
include(FetchContent)

# SOURCE_SUBDIR points at a folder with no CMakeLists.txt, so these are
# only downloaded; we compile the files we need ourselves below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.22
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(imgui miniaudio)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)
# Stop GLFW including the system OpenGL headers; ImGui and src/Gl.h load what they need at runtime
target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)

add_library(miniaudio STATIC ${miniaudio_SOURCE_DIR}/miniaudio.c)
target_include_directories(miniaudio PUBLIC ${miniaudio_SOURCE_DIR})
target_link_libraries(miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# ---- App ----
add_executable(sdr-app
    src/main.cpp
    src/App.cpp
    src/AudioSink.cpp
    src/Dsp.cpp
    src/Receiver.cpp
    src/RtlSdrSource.cpp
    src/Waterfall.cpp)
target_link_libraries(sdr-app PRIVATE
    PkgConfig::RTLSDR
    imgui
    miniaudio
    IPP::ipps IPP::ippvm IPP::ippcore
    Threads::Threads)
target_compile_definitions(sdr-app PRIVATE
    SDR_FONT_PATH="${CMAKE_SOURCE_DIR}/assets/fonts/CascadiaMono-Regular.ttf")
