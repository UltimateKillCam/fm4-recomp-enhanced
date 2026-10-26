# fm4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fm4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FM4_SOURCES
    src/main.cpp
    src/hooks.cpp
    src/native_hot.cpp
    src/native_tiling_hooks.cpp
    src/fm4_app.cpp
)

if(WIN32)
    list(APPEND FM4_SOURCES src/wheel_host.cpp src/wheel_driver.cpp src/wheel_hooks.cpp)
    add_executable(fm4 WIN32 ${FM4_SOURCES})
    target_link_libraries(fm4 PRIVATE dinput8 dxguid ole32 user32)
    # Read-only DirectInput reader for the launcher's live control binding page.
    add_executable(fm4-input src/input_capture.cpp)
    target_compile_features(fm4-input PRIVATE cxx_std_20)
    target_link_libraries(fm4-input PRIVATE dinput8 dxguid ole32 user32)
else()
    add_executable(fm4 ${FM4_SOURCES})
endif()

rexglue_setup_target(fm4 GPU_PLUGINS xenos)

option(FM4_VIP_CONTENT "Build optional local VIP ownership and content inspection" OFF)
if(FM4_VIP_CONTENT)
    target_sources(fm4 PRIVATE src/content_hooks.cpp)
endif()

option(FM4_ENABLE_MODS "Enable optional native mod application/game hooks" OFF)
if(FM4_ENABLE_MODS)
    if(NOT WIN32)
        message(FATAL_ERROR "FM4 mod host currently requires Windows")
    endif()
    set_property(SOURCE src/main.cpp src/fm4_app.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS FM4_ENABLE_MODS=1)
    set_property(SOURCE src/hooks.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS FM4_MOD_GAME_HOOKS=1)
endif()

# Opt-in native data adaptation. Generated PPC modules/entry configuration
# remain original. Review the separate receipt installer before enabling.
option(FM4_ENABLE_FORZA_PLUS_NATIVE "Build native Forza Plus receipt/resource hook" OFF)
if(FM4_ENABLE_FORZA_PLUS_NATIVE)
    if(NOT WIN32)
        message(FATAL_ERROR "Current native Forza Plus verifier requires Windows CNG/file leases")
    endif()
    find_path(FM4_PFP_JSON_INCLUDE_DIR nlohmann/json.hpp HINTS
        "${REXSDK_DIR}/thirdparty/inja/third_party/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../.local/rexglue-enhanced-sdk/thirdparty/inja/third_party/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../.local/rexglue-sdk/thirdparty/inja/third_party/include")
    if(NOT FM4_PFP_JSON_INCLUDE_DIR)
        # Official v3.11.3 single header and its MIT notice, both content pinned.
        set(FM4_PFP_JSON_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/fm4-pfp-json/include")
        file(MAKE_DIRECTORY "${FM4_PFP_JSON_INCLUDE_DIR}/nlohmann")
        file(DOWNLOAD "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
            "${FM4_PFP_JSON_INCLUDE_DIR}/nlohmann/json.hpp"
            EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6 TLS_VERIFY ON)
        file(DOWNLOAD "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT"
            "${FM4_PFP_JSON_INCLUDE_DIR}/nlohmann/LICENSE.MIT"
            EXPECTED_HASH SHA256=c983d69523377819db3c377b390d5644f5ec53bec9b7c4a0f1ed893bb743d045 TLS_VERIFY ON)
    endif()
    # These are the only two current TUs including fm4_app.h. Avoid changing
    # command lines for the thousands of generated PPC source files.
    set_property(SOURCE src/main.cpp src/fm4_app.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS FM4_ENABLE_FORZA_PLUS_NATIVE=1)
    set_property(SOURCE src/fm4_app.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${FM4_PFP_JSON_INCLUDE_DIR}")
    target_link_libraries(fm4 PRIVATE bcrypt)
    add_executable(fm4-pfp-resource-export src/forza_plus/resource_export.cpp)
    target_compile_features(fm4-pfp-resource-export PRIVATE cxx_std_23)
    target_link_libraries(fm4-pfp-resource-export PRIVATE rex::runtime bcrypt)
endif()

set(FM4_REGISTER_CPP
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fm4_register.cpp"
)
set(FM4_INIT_CPP
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fm4_init.cpp"
)
set(FM4_REGISTER_SPLIT_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/fm4_register.split.stamp"
)

# FM4 code generation emits an unusually large registrar. Split it
# automatically before every compile where the generated source is newer than
# the stamp.
add_custom_command(
    OUTPUT "${FM4_REGISTER_SPLIT_STAMP}"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/../Split FM4 Registrar.ps1"
            -Path "${FM4_REGISTER_CPP}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${FM4_REGISTER_SPLIT_STAMP}"
    DEPENDS
        "${FM4_REGISTER_CPP}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Split FM4 Registrar.ps1"
    VERBATIM
)
add_custom_target(fm4_split_register DEPENDS "${FM4_REGISTER_SPLIT_STAMP}")
add_dependencies(fm4 fm4_split_register)

# The split turns the generated 292k-call registrar into bounded noinline
# helpers, allowing clang to compile at O0 without one invalid giant function.
set_source_files_properties(
    "${FM4_REGISTER_CPP}"
    PROPERTIES
        COMPILE_OPTIONS "-O0"
        SKIP_PRECOMPILE_HEADERS ON
)

# The generated dispatcher is also large enough for release optimization to
# exhaust memory on a busy development machine. It is startup-only glue, so
# compiling it at O0 has no effect on recompiled game-code performance.
set_source_files_properties(
    "${FM4_INIT_CPP}"
    PROPERTIES
        COMPILE_OPTIONS "-O0"
        SKIP_PRECOMPILE_HEADERS ON
)
