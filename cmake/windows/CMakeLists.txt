# Windows x86-64 build of the SDL3 / Vulkan front end: the same sources as the Linux window build
# (tools/build_window.sh: the SDK runtime without its D3D12 / XAudio2 / Win32 app layer, the harness's app side and
# reference renderer, src/*), cross-compiled with cmake/toolchains/x86_64-w64-mingw32-clang.cmake.
# The generated code comes from the Linux recompiler run (tools/iterate.sh writes build/gen/game); the third-party
# binaries from tools/fetch_windows_deps.sh. tools/build_windows.sh configures, builds and packages.
cmake_minimum_required(VERSION 3.25)
project(csx_windows LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32-clang.cmake")
endif()

get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(CSX_GEN_DIR ${REPO}/build/gen/game CACHE PATH "generated code of the image (xbe_recomp output)")
set(CSX_DEPS_DIR ${REPO}/build/deps CACHE PATH "tools/fetch_windows_deps.sh output")
set(CSX_VULKAN_HEADERS ${REPO}/third_party/Vulkan-Headers/include CACHE PATH "Vulkan-Headers include folder")
set(SDK ${REPO}/sdk)
set(RT ${SDK}/runtime)
set(SDL3_DIR ${CSX_DEPS_DIR}/SDL3-3.4.18/x86_64-w64-mingw32)
set(DXC_DIR ${CSX_DEPS_DIR}/dxc)
set(SODIUM_DIR ${CSX_DEPS_DIR}/libsodium-win64)
foreach(p ${CSX_GEN_DIR}/x86_func_mapping.cpp ${SDL3_DIR}/lib/libSDL3.dll.a ${DXC_DIR}/lib/x64/dxcompiler.lib
          ${SODIUM_DIR}/lib/libsodium.a
          ${CSX_VULKAN_HEADERS}/vulkan/vulkan.h)
    if(NOT EXISTS ${p})
        message(FATAL_ERROR "missing ${p} (tools/iterate.sh, tools/fetch_windows_deps.sh)")
    endif()
endforeach()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# as sdk/harness/build.sh: the generated code at -O1 without debug info, the host code at the window build's -O3
set(BASE_FLAGS -march=x86-64-v2 -fno-strict-aliasing)

# generated code, compiled as on Linux
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${CSX_GEN_DIR}/x86_recomp.*.cpp)
add_library(csx_gen OBJECT ${GEN_SOURCES} ${CSX_GEN_DIR}/x86_func_mapping.cpp)
target_include_directories(csx_gen PRIVATE ${RT} ${CSX_GEN_DIR})
target_compile_options(csx_gen PRIVATE ${BASE_FLAGS} -O1 -g0 -w)

# host sources: the list tools/build_window.sh compiles
file(GLOB RT_SOURCES CONFIGURE_DEPENDS ${RT}/*/*.cpp)
list(FILTER RT_SOURCES EXCLUDE REGEX "/runtime/app/|/core/input\\.cpp$|/gpu/d3d12\\.cpp$|/audio/xaudio2\\.cpp$")
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${REPO}/src/*/*.cpp)
set(HARNESS_SOURCES ${SDK}/harness/app_headless.cpp ${SDK}/harness/reference_renderer.cpp)

add_executable(CounterStrike ${RT_SOURCES} ${HOST_SOURCES} ${HARNESS_SOURCES} $<TARGET_OBJECTS:csx_gen>)
target_include_directories(CounterStrike PRIVATE ${REPO}/src ${RT} ${CSX_GEN_DIR} ${SDK}/harness ${SDL3_DIR}/include
                                                 ${CSX_VULKAN_HEADERS} ${SODIUM_DIR}/include)
target_compile_definitions(CounterStrike PRIVATE XBE_RAW_VERTICES=0 XBE_RENDER_THREAD=0 UNICODE _UNICODE
                                                 _CRT_SECURE_NO_WARNINGS SODIUM_STATIC)
target_compile_options(CounterStrike PRIVATE ${BASE_FLAGS} -O3 -g)

# the Vulkan loader's import library, from the exports the backend uses
set(VULKAN_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/libvulkan-1.a)
add_custom_command(OUTPUT ${VULKAN_IMPLIB}
    COMMAND ${CMAKE_DLLTOOL} -m i386:x86-64 -d ${CMAKE_CURRENT_SOURCE_DIR}/vulkan-1.def -l ${VULKAN_IMPLIB}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vulkan-1.def VERBATIM)
add_custom_target(vulkan_implib DEPENDS ${VULKAN_IMPLIB})
add_dependencies(CounterStrike vulkan_implib)

# the C and C++ runtimes linked in statically (no libstdc++ / libgcc / winpthread DLLs); UCRT comes with Windows 10+
target_link_options(CounterStrike PRIVATE -static -g)
target_link_libraries(CounterStrike PRIVATE ${SDL3_DIR}/lib/libSDL3.dll.a ${VULKAN_IMPLIB}
                      ${DXC_DIR}/lib/x64/dxcompiler.lib ${SODIUM_DIR}/lib/libsodium.a ws2_32 pthread)

# package: the exe, the DLLs it loads that Windows does not have, licenses
install(TARGETS CounterStrike RUNTIME DESTINATION .)
install(FILES ${SDL3_DIR}/bin/SDL3.dll ${DXC_DIR}/bin/x64/dxcompiler.dll DESTINATION .)
install(FILES ${CSX_DEPS_DIR}/SDL3-3.4.18/LICENSE.txt DESTINATION licenses RENAME SDL3-LICENSE.txt)
install(FILES ${DXC_DIR}/LICENSE-LLVM.txt ${DXC_DIR}/LICENSE-MIT.txt ${DXC_DIR}/LICENSE-MS.txt DESTINATION licenses/dxc)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/run_game.bat ${CMAKE_CURRENT_SOURCE_DIR}/README.txt DESTINATION .)
