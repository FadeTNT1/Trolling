cmake_minimum_required(VERSION 3.20)
project(WD42 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Fetch Dear ImGui (docking branch) ─────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ── ImGui static library ──────────────────────────────────────────────
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC
    d3d11
    d3dcompiler
    dxgi
    dwmapi
)

# ── Main executable ───────────────────────────────────────────────────
add_executable(WD42
    src/main.cpp
    src/overlay.cpp
    src/mc_process.cpp
    src/scanner.cpp
    src/entity.cpp
    src/esp.cpp
)
target_link_libraries(WD42 PRIVATE imgui_lib)
target_include_directories(WD42 PRIVATE src)
