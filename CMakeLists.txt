cmake_minimum_required(VERSION 3.14)
project(ekit VERSION 0.1.0 LANGUAGES CXX)

option(EKIT_BUILD_TESTS "Build the ekit test suite" ON)
option(EKIT_BUILD_EXAMPLES "Build the ekit examples" ON)
option(EKIT_BUILD_BOIDS "Build the boids case study" ON)
option(EKIT_BUILD_BOIDS_LIVE "Build the boids GLFW live viewer (needs local GLFW)" ON)
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
if (EKIT_BUILD_EXAMPLES)
    add_executable(ekit_basic examples/basic.cpp)
    target_link_libraries(ekit_basic PRIVATE ekit)
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
    add_executable(ekit_tests tests/tests.cpp)
    target_link_libraries(ekit_tests PRIVATE ekit)
    add_test(NAME ekit_tests COMMAND ekit_tests)
endif()
