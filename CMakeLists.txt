cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23) # Geode v5 requires C++23; the core itself is C++20-clean
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
# Geode mods use the dynamic CRT on Windows; keep the static core on the same runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

project(AutoLayout VERSION 1.0.0 LANGUAGES C CXX)

# AL_CORE_ONLY builds the portable core, the CLI and the test suite without Geode
# (used for development, training and headless CI). It is the default when GEODE_SDK
# is not set.
if (NOT DEFINED AL_CORE_ONLY)
    if (DEFINED ENV{GEODE_SDK})
        set(AL_CORE_ONLY OFF)
    else()
        set(AL_CORE_ONLY ON)
    endif()
endif()

add_subdirectory(core)

if (AL_CORE_ONLY)
    message(STATUS "AutoLayout: building core, tools and tests only (no Geode)")
    enable_testing()
    add_subdirectory(tools)
    add_subdirectory(tests)
    return()
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} autolayout_core)
target_include_directories(${PROJECT_NAME} PRIVATE src)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
