cmake_minimum_required(VERSION 3.14)
project(ekit VERSION 0.1.0 LANGUAGES CXX)

option(EKIT_BUILD_TESTS "Build the ekit test suite" ON)
option(EKIT_BUILD_QUERY_BENCHMARK "Build the sparse query coverage benchmark" OFF)
set(EKIT_TOMCAT_SCENE_WORLD_DIR "" CACHE PATH "Optional directory containing TomCat dev_ekit's SceneWorld.h")
option(EKIT_BUILD_EXAMPLES "Build the ekit examples" ON)
option(EKIT_BUILD_BOIDS "Build the boids case study" ON)
option(EKIT_BUILD_BOIDS_LIVE "Build the boids GLFW live viewer (needs local GLFW)" ON)
option(EKIT_BUILD_ENTT_COMPARE "Build the ekit vs EnTT comparison benchmark (needs local EnTT)" ON)
set(EKIT_GLFW_ROOT "" CACHE PATH "Path to a local GLFW checkout (NOT part of this repo)")

# ---------------------------------------------------------------------------
# Header-only library
# ---------------------------------------------------------------------------
add_library(ekit INTERFACE)
add_library(ekit::ekit ALIAS ekit)

target_include_directories(ekit INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(ekit INTERFACE cxx_std_20)

# std::thread / std::atomic
find_package(Threads REQUIRED)
target_link_libraries(ekit INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if (EKIT_BUILD_QUERY_BENCHMARK)
    add_executable(ekit_sparse_query_bench benchmarks/sparse_query.cpp)
    target_link_libraries(ekit_sparse_query_bench PRIVATE ekit)
    add_executable(ekit_access_paths_bench benchmarks/access_paths.cpp)
    target_link_libraries(ekit_access_paths_bench PRIVATE ekit)
    add_executable(ekit_random_create_bench benchmarks/random_create.cpp)
    target_link_libraries(ekit_random_create_bench PRIVATE ekit)
    if (EKIT_TOMCAT_SCENE_WORLD_DIR)
        add_executable(ekit_tomcat_scene_bench benchmarks/tomcat_scene_world.cpp)
        target_link_libraries(ekit_tomcat_scene_bench PRIVATE ekit)
        target_include_directories(ekit_tomcat_scene_bench PRIVATE "${EKIT_TOMCAT_SCENE_WORLD_DIR}")
    endif()
endif()

if (EKIT_BUILD_EXAMPLES)
    add_executable(ekit_basic examples/basic.cpp)
    target_link_libraries(ekit_basic PRIVATE ekit)

    add_executable(ekit_ergonomic examples/ergonomic.cpp)
    target_link_libraries(ekit_ergonomic PRIVATE ekit)
endif()

# ---------------------------------------------------------------------------
# Boids case study
# ---------------------------------------------------------------------------
if (EKIT_BUILD_BOIDS)
    add_subdirectory(examples/boids)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if (EKIT_BUILD_TESTS)
    enable_testing()
    add_executable(ekit_tests tests/tests.cpp tests/sparse_lifetime.cpp tests/sparse_query.cpp tests/paged_storage.cpp)
    target_link_libraries(ekit_tests PRIVATE ekit)
    add_test(NAME ekit_tests COMMAND ekit_tests)
endif()
