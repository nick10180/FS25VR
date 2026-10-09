cmake_minimum_required(VERSION 3.20)
project(fs25vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OPENXR_DIR ${CMAKE_SOURCE_DIR}/third_party/openxr)

# The release version comes from the mod's modDesc.xml, so the log always names the version.
set(MODDESC ${CMAKE_SOURCE_DIR}/mod/FS25_VR/modDesc.xml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MODDESC})
file(READ ${MODDESC} MODDESC_TEXT)
string(REGEX MATCH "<version>([^<]+)</version>" _ "${MODDESC_TEXT}")
set(FS25VR_VERSION "${CMAKE_MATCH_1}")

add_library(dinput8 SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/log.cpp
    src/config.cpp
    src/hooks.cpp
    src/game.cpp
    src/xr.cpp
    src/overlay.cpp
    src/planes.cpp
    src/blit.cpp
    src/window.cpp
    src/sync.cpp
    src/hud.cpp
    src/profile.cpp
    src/dinput8.def
)

target_include_directories(dinput8 PRIVATE ${OPENXR_DIR}/include src)
target_compile_definitions(dinput8 PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D12
    FS25VR_VERSION="${FS25VR_VERSION}")
if(MSVC)
    target_compile_options(dinput8 PRIVATE /W3 /EHsc /Zi)
    target_link_options(dinput8 PRIVATE /DEBUG /OPT:REF)
    target_link_libraries(dinput8 PRIVATE
        ${OPENXR_DIR}/x64/lib/openxr_loader.lib
        d3d12 dxgi dxguid version)
    # The loader is delay-loaded from our own folder so the proxy never fails to load if it is missing.
    target_link_options(dinput8 PRIVATE /DELAYLOAD:openxr_loader.dll)
    target_link_libraries(dinput8 PRIVATE delayimp)
else()
    # MinGW cross build (e.g. llvm-mingw on Linux, see cmake/mingw-toolchain.cmake). The import
    # library is generated from the release's openxr_loader.dll; the C++ runtime is linked
    # statically so the DLL needs nothing beyond what the game folder already has.
    target_compile_options(dinput8 PRIVATE -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
    target_link_options(dinput8 PRIVATE -static -Wl,--kill-at)
    target_link_libraries(dinput8 PRIVATE
        ${OPENXR_DIR}/mingw/libopenxr_loader.a
        d3d12 dxgi dxguid version)
    set_target_properties(dinput8 PROPERTIES PREFIX "")
endif()

set_target_properties(dinput8 PROPERTIES OUTPUT_NAME "dinput8")
