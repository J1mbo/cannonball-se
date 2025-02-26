# -----------------------------------------------------------------------------
# CannonBall Linux Setup
# -----------------------------------------------------------------------------

# Use OpenGL for rendering.
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# Platform Specific Libraries
set(platform_link_libs
    ${OPENGL_LIBRARIES}
    SDL2
    SDL2_gpu
    udev
)

# GCC Specific flags (optimise for speed and with the current CPU)
set(CMAKE_CXX_FLAGS "-march=native -fopenmp")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")

# Generate optimised code
set(CMAKE_BUILD_TYPE Release)

# Standards
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

