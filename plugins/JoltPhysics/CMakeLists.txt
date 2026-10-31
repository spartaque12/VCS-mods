# JoltPhysics.asi: ragdolls, bike falls, hit reactions, dismemberment, pickups on the ground, foot IK, jiggle physics - on Jolt
# Physics 5.6.0 (third_party/JoltPhysics, MIT, vendored unmodified: Jolt/ and Build/CMakeLists.txt of the v5.6.0 tag).
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)                 # the project's release flags, not Jolt's own
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)  # never trap inside the game process
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)    # /MD like vcs.exe (its default is /MT)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)                # its /Wall /WX is -Weverything under clang-cl
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)                   # the map is 4 km wide: floats hold it to 0.25 mm
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)                       # no GPU compute (its hair)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
# vcs.exe's baseline, x86-64-v3 (AVX2, FMA, BMI, LZCNT, F16C); no AVX-512
set(USE_SSE4_1 ON CACHE BOOL "" FORCE)
set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
set(USE_AVX ON CACHE BOOL "" FORCE)
set(USE_AVX2 ON CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT ON CACHE BOOL "" FORCE)
set(USE_TZCNT ON CACHE BOOL "" FORCE)
set(USE_F16C ON CACHE BOOL "" FORCE)
set(USE_FMADD ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/JoltPhysics/Build" "${CMAKE_BINARY_DIR}/jolt" EXCLUDE_FROM_ALL)
set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The only code that includes Jolt: its defines and instruction-set options are PUBLIC on its target and stay here.
add_library(jolt_ragdoll STATIC src/physics/ragdoll.cpp)
target_include_directories(jolt_ragdoll PUBLIC src)
target_link_libraries(jolt_ragdoll PRIVATE Jolt)
# Jolt is built without RTTI; classes derived from its own here must be too (their typeinfo would name Jolt's bases)
if(MSVC)
    target_compile_options(jolt_ragdoll PRIVATE /GR-)
else()
    target_compile_options(jolt_ragdoll PRIVATE -fno-rtti)
endif()

vcs_add_mod(JoltPhysics
    src/main.cpp
    src/jolt.cpp
    src/footik.cpp
    src/jiggle.cpp
)
target_link_libraries(JoltPhysics PRIVATE jolt_ragdoll)
