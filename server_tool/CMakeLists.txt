# csx-server-tool: the dedicated server tool (docs/server_tool.md). A small project of its own, apart from the game's
# builds (it contains no game code): Dear ImGui (the SDK's version) on SDL3 with SDL_Renderer, plus the server's own
# settings rules (src/server/server_config.cpp, pure C++).
#   Linux:   cmake -S server_tool -B build/server_tool -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/server_tool
#   Windows: tools/build_server_tool.sh windows   (cross-built with the MinGW toolchain and build/deps' SDL3)
cmake_minimum_required(VERSION 3.20)
project(csx_server_tool CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

# Dear ImGui: a local copy (-DIMGUI_DIR, e.g. the SDK build's) or v1.91.0 fetched as the SDK fetches it
set(IMGUI_DIR "" CACHE PATH "Dear ImGui source folder (empty: fetch v1.91.0)")
if(NOT IMGUI_DIR)
    include(FetchContent)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR ${imgui_SOURCE_DIR})
endif()

find_package(SDL3 REQUIRED CONFIG)

set(IMGUI_SOURCES ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)

# the parts without a window (unit-tested)
add_library(csx_tool_core STATIC
    src/controller.cpp src/gamefiles.cpp src/json.cpp src/logs.cpp src/netaddr.cpp src/server_process.cpp src/sha256.cpp
    src/storage.cpp src/sysstats.cpp src/telemetry.cpp src/script.cpp ${REPO}/src/server/server_config.cpp)
target_include_directories(csx_tool_core PUBLIC src ${REPO}/src)
target_link_libraries(csx_tool_core PUBLIC SDL3::SDL3)
if(WIN32)
    target_link_libraries(csx_tool_core PUBLIC iphlpapi ws2_32 psapi pthread)
    # the C and C++ runtimes linked in statically, as the game's Windows build (no libstdc++ / winpthread DLLs)
    target_link_options(csx_tool_core INTERFACE -static)
endif()

# ImGui 1.91.0's SDL3 backends predate SDL 3.2's renames (SDL_TRUE): SDL's own compatibility names for them
set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    PROPERTIES COMPILE_DEFINITIONS SDL_ENABLE_OLD_NAMES)
add_executable(csx-server-tool src/main.cpp src/theme.cpp src/ui.cpp src/ui_pages.cpp src/ui_console.cpp ${IMGUI_SOURCES})
target_include_directories(csx-server-tool PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(csx-server-tool PRIVATE csx_tool_core)
if(WIN32)
    set_target_properties(csx-server-tool PROPERTIES WIN32_EXECUTABLE ON)
endif()

add_executable(csx-server-tool-tests tests/unit_test.cpp)
target_link_libraries(csx-server-tool-tests PRIVATE csx_tool_core)
