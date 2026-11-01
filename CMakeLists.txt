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

# Shaders of render mods (api/vcs_mod_api.h "Rendering"): one HLSL source, compiled at build time into headers - DXBC by
# fxc (Windows SDK) for D3D11: shaders/dxbc/<name>.h, `const BYTE <name>[]`; SPIR-V by glslang (Vulkan SDK, or the
# glslang package on Linux) for Vulkan: shaders/spirv/<name>.h, `const uint32_t <name>_spv[]`, with the port's bindings
# (bN -> N, tN -> 16 + N, sN -> 32 + N, set 0). The target gets VCS_MOD_HAVE_DXBC / VCS_MOD_HAVE_SPIRV for the kinds built.
#   vcs_mod_shader(<target> <file.hlsl> <entry> <vs_5_0|ps_5_0> <name> [NAME=VALUE defines...])
if(WIN32)
    file(GLOB _vcs_fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    list(SORT _vcs_fxc_candidates COMPARE NATURAL ORDER DESCENDING)
    find_program(VCS_MOD_FXC fxc HINTS ${_vcs_fxc_candidates})
endif()
find_program(VCS_MOD_GLSLANG NAMES glslangValidator glslang HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
function(vcs_mod_shader target source entry profile name)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    if(VCS_MOD_FXC)
        set(defines "")
        foreach(d IN LISTS ARGN)
            list(APPEND defines /D ${d})
        endforeach()
        add_custom_command(
            OUTPUT "${out}/dxbc/${name}.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${out}/dxbc"
            COMMAND "${VCS_MOD_FXC}" /nologo /O3 /T ${profile} /E ${entry} /Vn ${name} ${defines} /Fh "${out}/dxbc/${name}.h" "${src}"
            DEPENDS "${src}"
            COMMENT "fxc ${source} ${entry} -> ${name}"
            VERBATIM)
        target_sources(${target} PRIVATE "${out}/dxbc/${name}.h")
        target_compile_definitions(${target} PRIVATE VCS_MOD_HAVE_DXBC=1)
    endif()
    if(VCS_MOD_GLSLANG)
        string(SUBSTRING "${profile}" 0 2 kind)
        if(kind STREQUAL "vs")
            set(stage vert)
        else()
            set(stage frag)
        endif()
        set(defines "")
        foreach(d IN LISTS ARGN)
            list(APPEND defines -D${d})
        endforeach()
        # -D alone: the input is HLSL; the entry point is "main" in every module, as the port's tools/hlsl_spirv.py
        add_custom_command(
            OUTPUT "${out}/spirv/${name}.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${out}/spirv"
            COMMAND "${VCS_MOD_GLSLANG}" -D -V --target-env vulkan1.3 --shift-texture-binding 16 --shift-sampler-binding 32
                    -DVCS_SPIRV=1 ${defines} -S ${stage} -e main --source-entrypoint ${entry} --vn ${name}_spv
                    -o "${out}/spirv/${name}.h" "${src}"
            DEPENDS "${src}"
            COMMENT "glslang ${source} ${entry} -> ${name}_spv"
            VERBATIM)
        target_sources(${target} PRIVATE "${out}/spirv/${name}.h")
        target_compile_definitions(${target} PRIVATE VCS_MOD_HAVE_SPIRV=1)
    endif()
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

add_subdirectory(plugins/CameraAim)
add_subdirectory(plugins/JoltPhysics)
add_subdirectory(plugins/Project2DFX)
add_subdirectory(plugins/RenderHook)
add_subdirectory(plugins/VR)
add_subdirectory(plugins/PedSkills)
add_subdirectory(plugins/WeaponHD)
