# Cross build of dinput8.dll on Linux with llvm-mingw (or another x86_64-w64-mingw32 toolchain):
#   cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mingw
# OpenXR: headers in third_party/openxr/include, import library third_party/openxr/mingw/libopenxr_loader.a
# (llvm-dlltool -m i386:x86-64 -d openxr_loader.def -l libopenxr_loader.a, with the .def listing the
# exports of the release's openxr_loader.dll).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "toolchain prefix")
find_program(MINGW_CXX NAMES ${MINGW_PREFIX}-clang++ ${MINGW_PREFIX}-g++
             PATHS /opt/llvm-mingw/bin /usr/bin REQUIRED)
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
find_program(MINGW_RC NAMES ${MINGW_PREFIX}-windres llvm-windres PATHS /opt/llvm-mingw/bin /usr/bin)
if(MINGW_RC)
    set(CMAKE_RC_COMPILER ${MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
