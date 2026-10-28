# libautolayout: the portable core (no Geode / cocos dependency).
file(GLOB_RECURSE AL_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(autolayout_core STATIC
    ${AL_CORE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c)

target_include_directories(autolayout_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minimp3
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)

target_compile_features(autolayout_core PUBLIC cxx_std_20)
set_target_properties(autolayout_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (WIN32)
    # minimp3 includes windows.h: keep its min/max macros out of our code.
    target_compile_definitions(autolayout_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if (NOT (MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    # Third-party C sources are noisy; keep our own code warning-clean.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/stb_vorbis_impl.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Deterministic floating point: no fast-math, no FMA contraction (physics must match across platforms).
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(autolayout_core PRIVATE -ffp-contract=off)
endif()
if (MSVC)
    target_compile_options(autolayout_core PRIVATE /fp:precise)
endif()
