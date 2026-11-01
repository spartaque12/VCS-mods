# RenderHook.asi: RenderHook-style graphics in the port's renderer through the mod API's render hooks - ambient occlusion
# (GTAO) first.
vcs_add_mod(RenderHook
    src/main.cpp
    src/renderhook.cpp
    src/temporal.cpp
    src/shadows.cpp
    src/blobs.cpp
    src/water.cpp
    src/water_sim.cpp # stage 3: wakes and ripples
    src/water_shore.cpp # stage 2: shore foam, shore waves, caustics
)
vcs_mod_shader(RenderHook shaders/gtao.hlsl vs_main vs_5_0 kRhFullScreenVS)
vcs_mod_shader(RenderHook shaders/gtao.hlsl ps_view_depth ps_5_0 kRhViewDepthPS)
vcs_mod_shader(RenderHook shaders/gtao.hlsl ps_view_depth ps_5_0 kRhViewDepthMsPS MS=1)
vcs_mod_shader(RenderHook shaders/gtao.hlsl ps_gtao ps_5_0 kRhGtaoPS)
vcs_mod_shader(RenderHook shaders/gtao.hlsl ps_apply ps_5_0 kRhApplyPS)
vcs_mod_shader(RenderHook shaders/gtao.hlsl ps_show ps_5_0 kRhShowPS)
# temporal AA groundwork: motion vectors and the reprojection check (debug views; DLAA later)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_motion_view ps_5_0 kRhMotionViewPS)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_motion_view ps_5_0 kRhMotionViewMsPS MS=1)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_reproject ps_5_0 kRhReprojectPS)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_reproject ps_5_0 kRhReprojectMsPS MS=1)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_copy ps_5_0 kRhCopyPS)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_dlaa_depth ps_5_0 kRhDlaaDepthPS)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_dlaa_depth ps_5_0 kRhDlaaDepthMsPS MS=1)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_dlaa_motion ps_5_0 kRhDlaaMotionPS)
vcs_mod_shader(RenderHook shaders/temporal.hlsl ps_dlaa_motion ps_5_0 kRhDlaaMotionMsPS MS=1)
# sun shadows: cascaded shadow maps of the scene's draws (render_shadow_map)
vcs_mod_shader(RenderHook shaders/shadows.hlsl ps_shadow ps_5_0 kRhShadowPS)
vcs_mod_shader(RenderHook shaders/shadows.hlsl ps_shadow ps_5_0 kRhShadowMsPS MS=1)
vcs_mod_shader(RenderHook shaders/shadows.hlsl ps_shadow_show ps_5_0 kRhShadowShowPS)
vcs_mod_shader(RenderHook shaders/shadows.hlsl ps_shadow_show ps_5_0 kRhShadowShowMsPS MS=1)
# water (docs/RENDERHOOK_PLAN.md "Water"): the surface at VCS_RENDER_AFTER_WATER
vcs_mod_shader(RenderHook shaders/water.hlsl ps_raw_depth ps_5_0 kRhWaterRawDepthPS)
vcs_mod_shader(RenderHook shaders/water.hlsl ps_raw_depth ps_5_0 kRhWaterRawDepthMsPS MS=1)
vcs_mod_shader(RenderHook shaders/water.hlsl ps_info ps_5_0 kRhWaterInfoPS)
vcs_mod_shader(RenderHook shaders/water.hlsl ps_info ps_5_0 kRhWaterInfoMsPS MS=1)
vcs_mod_shader(RenderHook shaders/water.hlsl ps_extras_clear ps_5_0 kRhWaterExtrasClearPS)
vcs_mod_shader(RenderHook shaders/water.hlsl ps_water ps_5_0 kRhWaterPS)
vcs_mod_shader(RenderHook shaders/water.hlsl ps_water_show ps_5_0 kRhWaterShowPS)
# stage 3: wakes and ripples (src/water_sim.cpp)
vcs_mod_shader(RenderHook shaders/water_sim.hlsl ps_wake_step ps_5_0 kRhWakeStepPS)
vcs_mod_shader(RenderHook shaders/water_sim.hlsl ps_wake_out ps_5_0 kRhWakeOutPS)
vcs_mod_shader(RenderHook shaders/water_sim.hlsl ps_wake_show ps_5_0 kRhWakeShowPS)
vcs_mod_shader(RenderHook shaders/water_sim.hlsl ps_wake_water ps_5_0 kRhWakeWaterPS)
vcs_mod_shader(RenderHook shaders/water_sim.hlsl ps_wake_water ps_5_0 kRhWakeWaterMsPS MS=1)
# stage 2: shore foam, shore waves, caustics (src/water_shore.cpp)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_shore_extras ps_5_0 kRhShoreExtrasPS)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_shore_show ps_5_0 kRhShoreShowPS)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_shore_show ps_5_0 kRhShoreShowMsPS MS=1)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_shore_swash ps_5_0 kRhShoreSwashPS)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_shore_swash ps_5_0 kRhShoreSwashMsPS MS=1)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_pool_caustics ps_5_0 kRhPoolCausticsPS)
vcs_mod_shader(RenderHook shaders/water_shore.hlsl ps_pool_caustics ps_5_0 kRhPoolCausticsMsPS MS=1)
# DLAA through NVIDIA NGX (the DLSS SDK, github.com/NVIDIA/DLSS: include\, lib\Windows_x86_64\x64\nvsdk_ngx_s.lib and
# lib\Windows_x86_64\rel\nvngx_dlss.dll - binaries, not in this repository). VCS_NGX_DIR = a folder with include\ and lib\
# (nvsdk_ngx_s.lib, nvngx_dlss.dll side by side); without it RenderHook.asi has no DLAA row.
set(VCS_NGX_DIR "" CACHE PATH "NVIDIA DLSS SDK folder with include/ and lib/ (nvsdk_ngx_s.lib, nvngx_dlss.dll)")
if(WIN32 AND VCS_NGX_DIR AND EXISTS "${VCS_NGX_DIR}/lib/nvsdk_ngx_s.lib")
    target_sources(RenderHook PRIVATE src/dlaa.cpp)
    target_include_directories(RenderHook PRIVATE "${VCS_NGX_DIR}/include")
    target_compile_definitions(RenderHook PRIVATE VCS_MOD_HAVE_NGX=1)
    # nvsdk_ngx_s.lib is built with the static CRT (/MT): so is RenderHook.asi then (its API to vcs.exe is plain C)
    set_property(TARGET RenderHook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_link_libraries(RenderHook PRIVATE "${VCS_NGX_DIR}/lib/nvsdk_ngx_s.lib" d3d11 dxgi version advapi32)
    add_custom_command(TARGET RenderHook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VCS_NGX_DIR}/lib/nvngx_dlss.dll" "$<TARGET_FILE_DIR:RenderHook>/"
        COMMENT "nvngx_dlss.dll beside RenderHook.asi")
else()
    target_sources(RenderHook PRIVATE src/dlaa_none.cpp)
endif()
