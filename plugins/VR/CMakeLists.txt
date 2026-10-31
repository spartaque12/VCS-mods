# VR.asi: the game in an OpenXR headset (docs/VR_PLAN.md M0 of the port): a session on vcs.exe's D3D11 device, frames
# paced by the headset, the 3D picture head-tracked in first person, the HUD and menus on panels.
#
# openxr_loader.dll (Khronos OpenXR-SDK, Apache-2.0) is loaded at run time from VR.asi's folder; with
# VR_BUILD_OPENXR_LOADER (the default on Windows) it is built here from OpenXR-SDK release-1.1.65 (FetchContent; an
# offline checkout: -DFETCHCONTENT_SOURCE_DIR_OPENXR_SDK=<dir>) and copied next to VR.asi.
# VR_TEST_RUNTIME: a minimal OpenXR runtime for tests without a headset (test/xr_test_runtime.cpp; never installed).
vcs_add_mod(VR
    src/main.cpp
    src/xr.cpp
)
target_include_directories(VR PRIVATE "${CMAKE_SOURCE_DIR}/third_party/openxr/include")
target_link_libraries(VR PRIVATE vcs_mod_imgui d3d11 dxgi)
vcs_mod_shader(VR shaders/vr_blit.hlsl vs_main vs_5_0 vr_vs)
vcs_mod_shader(VR shaders/vr_blit.hlsl ps_copy ps_5_0 vr_ps_copy)
vcs_mod_shader(VR shaders/vr_blit.hlsl ps_hud ps_5_0 vr_ps_hud)

option(VR_BUILD_OPENXR_LOADER "Build openxr_loader.dll (OpenXR-SDK) and put it next to VR.asi" ${WIN32})
if(VR_BUILD_OPENXR_LOADER)
    include(FetchContent)
    set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.65
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(openxr_sdk)
    add_dependencies(VR openxr_loader)
    add_custom_command(TARGET VR POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:openxr_loader>" "$<TARGET_FILE_DIR:VR>/"
        COMMENT "openxr_loader.dll next to VR.asi")
    if(VCS_RECOMP_DIR AND VCS_INSTALL_SCRIPTS)
        add_custom_command(TARGET VR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:openxr_loader>" "${VCS_RECOMP_DIR}/build/scripts/")
    endif()
endif()

option(VR_TEST_RUNTIME "Build xr_test_runtime.dll, an OpenXR runtime for tests without a headset" OFF)
if(VR_TEST_RUNTIME AND WIN32)
    add_library(xr_test_runtime SHARED test/xr_test_runtime.cpp)
    target_include_directories(xr_test_runtime PRIVATE "${CMAKE_SOURCE_DIR}/third_party/openxr/include")
    target_compile_definitions(xr_test_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(xr_test_runtime PRIVATE d3d11)
    add_custom_command(TARGET xr_test_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/test/xr_test_runtime.json" "$<TARGET_FILE_DIR:xr_test_runtime>/")
endif()
