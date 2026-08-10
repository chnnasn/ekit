# ekit Boids case study - build targets.

# Headless demo: runs the simulation and writes PPM frames (render.ps1 converts
# them to PNG / animated GIF).
add_executable(ekit_boids main.cpp)
target_link_libraries(ekit_boids PRIVATE ekit)
target_compile_features(ekit_boids PRIVATE cxx_std_20)

# Simulation benchmark: boid-count x thread-count matrix with parallel speedup.
add_executable(ekit_boids_bench bench.cpp)
target_link_libraries(ekit_boids_bench PRIVATE ekit)
target_compile_features(ekit_boids_bench PRIVATE cxx_std_20)

# ekit vs EnTT comparison: the SAME boids algorithm on both ECS libraries.
# EnTT is NOT part of this repository - point ENTT_ROOT at a local checkout,
# e.g. E:/Github/entt.
set(ENTT_ROOT "" CACHE PATH "Path to a local EnTT checkout (NOT part of this repo)")
if (EKIT_BUILD_ENTT_COMPARE AND EXISTS "${ENTT_ROOT}/single_include/entt/entt.hpp")
    add_executable(ekit_entt_compare compare_bench.cpp)
    target_link_libraries(ekit_entt_compare PRIVATE ekit)
    target_include_directories(ekit_entt_compare PRIVATE "${ENTT_ROOT}/single_include")
    target_compile_features(ekit_entt_compare PRIVATE cxx_std_20)
    # third-party header; keep it warning-tolerant
    if (MSVC)
        target_compile_options(ekit_entt_compare PRIVATE /O2)
    else()
        target_compile_options(ekit_entt_compare PRIVATE -O2)
    endif()
else()
    message(STATUS "ekit_entt_compare skipped: configure with -DENTT_ROOT=<local EnTT checkout>")
endif()

# Real-time viewer: GLFW + OpenGL (GPU rendering). GLFW is NOT part of this
# repository - pass -DEKIT_GLFW_ROOT=<path> pointing at a local GLFW checkout,
# e.g. E:/Github/glfw (the user's fork). SPACE pause, R reset, UP/DOWN speed,
# ESC quit.
if (EKIT_BUILD_BOIDS_LIVE AND EXISTS "${EKIT_GLFW_ROOT}/CMakeLists.txt")
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${EKIT_GLFW_ROOT}" glfw EXCLUDE_FROM_ALL)

    add_executable(ekit_boids_live main_live.cpp)
    target_link_libraries(ekit_boids_live PRIVATE ekit glfw)
    target_compile_features(ekit_boids_live PRIVATE cxx_std_20)

    find_package(OpenGL REQUIRED)
    target_link_libraries(ekit_boids_live PRIVATE OpenGL::GL)
else()
    message(STATUS "ekit_boids_live skipped: configure with -DEKIT_GLFW_ROOT=<local GLFW checkout>")
endif()

if (MSVC)
    target_compile_options(ekit_boids PRIVATE /O2 /W4 /WX)
    target_compile_options(ekit_boids_bench PRIVATE /O2 /W4 /WX)
    if (TARGET ekit_boids_live)
        target_compile_options(ekit_boids_live PRIVATE /O2 /W4 /WX)
    endif()
else()
    target_compile_options(ekit_boids PRIVATE -O2 -Wall -Wextra -Werror)
    target_compile_options(ekit_boids_bench PRIVATE -O2 -Wall -Wextra -Werror)
    if (TARGET ekit_boids_live)
        target_compile_options(ekit_boids_live PRIVATE -O2 -Wall -Wextra -Werror)
    endif()
endif()
