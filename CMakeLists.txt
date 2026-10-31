# Mods for the GTA: Vice City Stories native port (spartaque12/gta-vcs-recomp, branch vcs-mods), each built as its
# own DLL - an ".asi" vcs.exe loads from build\scripts\ (README.md, api/vcs_mod_api.h).
#
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Hooks on the game's recompiled functions use its calling convention (clang's preserve_none): clang / clang-cl only.
cmake_minimum_required(VERSION 3.20)
project(VCSMods LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# vcs.exe's C runtime (the DLL one): one heap and one stderr for both sides
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The mods hook the game's recompiled functions, which use clang's preserve_none calling "
                        "convention: configure with clang-cl (Windows) or clang (Linux).")
endif()

# The port's checkout / release folder (optional): its API header is compared with api/ at configure time, and with
# VCS_INSTALL_SCRIPTS=ON each built mod is copied into its build/scripts.
set(VCS_RECOMP_DIR "" CACHE PATH "The VCS native port's folder (optional)")
option(VCS_INSTALL_SCRIPTS "Copy the built mods into VCS_RECOMP_DIR/build/scripts" OFF)
if(VCS_RECOMP_DIR)
    set(_port_api "${VCS_RECOMP_DIR}/src/modapi/vcs_mod_api.h")
    if(EXISTS "${_port_api}")
        file(READ "${_port_api}" _port_text)
        file(READ "${CMAKE_SOURCE_DIR}/api/vcs_mod_api.h" _our_text)
        string(REPLACE "\r\n" "\n" _port_text "${_port_text}")
        string(REPLACE "\r\n" "\n" _our_text "${_our_text}")
        if(NOT _port_text STREQUAL _our_text)
            message(WARNING "api/vcs_mod_api.h differs from ${_port_api}: copy the port's header here if it is newer")
        endif()
    else()
        message(WARNING "${_port_api} not found: VCS_RECOMP_DIR is not a port with the mod API")
    endif()
endif()

# Dear ImGui, the exe's version and configuration (1.92.7-docking, no imconfig changes): a mod draws into vcs.exe's
# context (the crosshair), so the structures must match - api/vcs_mod_api.h hands the sizes over and the mod checks.
add_library(vcs_mod_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
)
target_include_directories(vcs_mod_imgui PUBLIC third_party/imgui)

add_library(vcs_mod_api INTERFACE)
target_include_directories(vcs_mod_api INTERFACE api)

# A mod: a shared library named <name>.asi on Windows (lib-less <name>.so on Linux).
function(vcs_add_mod name)
    add_library(${name} SHARED ${ARGN} "${CMAKE_SOURCE_DIR}/plugins/common/mod.cpp")
    target_link_libraries(${name} PRIVATE vcs_mod_api)
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/plugins/common")
    if(WIN32)
        set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".asi")
        target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        # vcs.exe's float flags: no FP contraction (bit-exact game maths), AVX2
        target_compile_options(${name} PRIVATE /arch:AVX2 /clang:-fno-math-errno /clang:-ffp-contract=off)
    else()
        set_target_properties(${name} PROPERTIES PREFIX "")
        target_compile_options(${name} PRIVATE -mavx2 -fno-math-errno -ffp-contract=off)
    endif()
    if(VCS_RECOMP_DIR AND VCS_INSTALL_SCRIPTS)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VCS_RECOMP_DIR}/build/scripts"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${name}>" "${VCS_RECOMP_DIR}/build/scripts/"
            COMMENT "${name} -> ${VCS_RECOMP_DIR}/build/scripts")
    endif()
endfunction()

add_subdirectory(plugins/CameraAim)
add_subdirectory(plugins/Project2DFX)
add_subdirectory(plugins/MeshSmooth)
