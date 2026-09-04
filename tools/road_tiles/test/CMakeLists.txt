# ------------------------------------------------------------------------------------
# Standalone test harness for the road/tile renderers (IMPLEMENTATION_SPEC.md S0).
#
# Compiles the REAL src/main/hwvideo/hwroad.cpp and hwtiles.cpp, completely
# unmodified by this project, together with the driver in this directory,
# against the stub frontend/config.hpp in ./stubs (placed first on the include
# path so it shadows the real one -- see stubs/frontend/config.hpp for why that
# is safe). This is never added as a subdirectory of, or linked into, the
# top-level cannonball-se target; configure and build it on its own, e.g.:
#
#   cmake -S tools/road_tiles/test -B tools/road_tiles/test/build
#   cmake --build tools/road_tiles/test/build -j
#   ./tools/road_tiles/test/build/road_tiles_test_scalar selftest
#
# Two targets, both must pass everywhere (S0.6):
#   road_tiles_test         - native ISA (add -mcpu=cortex-a53 on the Pi)
#   road_tiles_test_scalar  - -DCB_FORCE_SCALAR=1 (portable fallback paths only)
# ------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(road_tiles_test CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# tools/road_tiles/test -> repo root is three levels up.
get_filename_component(CB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CB_SRC_MAIN "${CB_REPO_ROOT}/src/main")

set(RT_SOURCES
    "${CB_SRC_MAIN}/hwvideo/hwroad.cpp"
    "${CB_SRC_MAIN}/hwvideo/hwtiles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/road_tiles_test.cpp"
)

function(road_tiles_add_target target_name)
    add_executable(${target_name} ${RT_SOURCES})

    # Stub include dir FIRST: "frontend/config.hpp" resolves to the 3-member
    # stub, not the real (SDL2/TinyXML2-dependent) one -- S0.6.
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
        "${CB_SRC_MAIN}"
    )

    target_compile_definitions(${target_name} PRIVATE
        CB_RENDER_TEST=1
        CB_RENDER_CHECKS=1
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /O2)
    else()
        target_compile_options(${target_name} PRIVATE -O2 -Wall)
    endif()
endfunction()

road_tiles_add_target(road_tiles_test)

road_tiles_add_target(road_tiles_test_scalar)
target_compile_definitions(road_tiles_test_scalar PRIVATE CB_FORCE_SCALAR=1)

# On the Pi (AArch64 Cortex-A53), tune the native-ISA target to the real core.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT MSVC)
    target_compile_options(road_tiles_test PRIVATE -mcpu=cortex-a53)
endif()
