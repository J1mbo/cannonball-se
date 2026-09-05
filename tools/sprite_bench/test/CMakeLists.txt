# ------------------------------------------------------------------------------------
# Standalone test harness for the sprite renderer (SPRITE_IMPLEMENTATION_SPEC.md S0).
#
# Compiles the REAL src/main/hwvideo/hwsprites.cpp, completely unmodified by
# this project, together with the driver in this directory, against the stub
# frontend/config.hpp in ./stubs (placed first on the include path so it
# shadows the real one -- see stubs/frontend/config.hpp for why that is
# safe). This is never added as a subdirectory of, or linked into, the
# top-level cannonball-se target; configure and build it on its own, e.g.:
#
#   cmake -S tools/sprite_bench/test -B tools/sprite_bench/test/build
#   cmake --build tools/sprite_bench/test/build -j
#   ./tools/sprite_bench/test/build/sprite_test_scalar selftest
#
# Two targets, both must pass everywhere (S0.7):
#   sprite_test         - native ISA (add -mcpu=cortex-a53 on the Pi)
#   sprite_test_scalar  - -DCB_FORCE_SCALAR=1 (portable fallback paths only)
# ------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(sprite_bench_test CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# tools/sprite_bench/test -> repo root is three levels up.
get_filename_component(CB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CB_SRC_MAIN "${CB_REPO_ROOT}/src/main")

set(SPR_SOURCES
    "${CB_SRC_MAIN}/hwvideo/hwsprites.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sprite_test.cpp"
)

function(sprite_test_add_target target_name)
    add_executable(${target_name} ${SPR_SOURCES})

    # Stub include dir FIRST: "frontend/config.hpp" resolves to the stub
    # (S0.7), not the real (SDL2/TinyXML2-dependent) one.
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
        "${CB_SRC_MAIN}"
    )

    target_compile_definitions(${target_name} PRIVATE
        CB_RENDER_TEST=1
        CB_SPRITE_STATS=1
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /O2)
    else()
        target_compile_options(${target_name} PRIVATE -O2 -Wall)
    endif()
endfunction()

sprite_test_add_target(sprite_test)

sprite_test_add_target(sprite_test_scalar)
target_compile_definitions(sprite_test_scalar PRIVATE CB_FORCE_SCALAR=1)

# On the Pi (AArch64 Cortex-A53), tune the native-ISA target to the real core.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT MSVC)
    target_compile_options(sprite_test PRIVATE -mcpu=cortex-a53)
endif()
