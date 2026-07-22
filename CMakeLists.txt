cmake_minimum_required(VERSION 3.10)
project(kraken VERSION 0.1.1 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force CMP0091 NEW + an explicit dynamic-DLL runtime library globally, before any
# add_subdirectory. JoltPhysics's own Build/CMakeLists.txt overrides CMAKE_CXX_FLAGS_DEBUG
# without an /MDd flag (see extern/joltphysics/Build/CMakeLists.txt ~line 191) and only
# sets CMAKE_MSVC_RUNTIME_LIBRARY when its own USE_STATIC_MSVC_RUNTIME_LIBRARY option is
# on (default off) - left alone, Jolt.lib silently links the release DLL CRT even in a
# Debug build, which LNK2038-mismatches against kraken.dll's explicit MDd/MD below.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)

# Platform check
if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only (Win32)")
endif()

add_subdirectory(extern/lim)
add_subdirectory(extern/hta)

# JoltPhysics — vendored for the [jolt] hybrid-vehicle-physics fix module.
# hta.exe is a 32-bit process; match Jolt's own official 32-bit build script
# (Build/cmake_vs2022_cl_32bit.bat) exactly, which keeps the whole build on an
# SSE2 baseline (no AVX/AVX2/FMA/LZCNT/TZCNT/F16C) — this is independent of the
# /arch:AVX2 flag kraken itself compiles with below, kept deliberately separate
# per docs/jolt-integration-techanalysis.md §3.6.
set(USE_SSE4_1             OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2             OFF CACHE BOOL "" FORCE)
set(USE_AVX                OFF CACHE BOOL "" FORCE)
set(USE_AVX2               OFF CACHE BOOL "" FORCE)
set(USE_AVX512             OFF CACHE BOOL "" FORCE)
set(USE_LZCNT              OFF CACHE BOOL "" FORCE)
set(USE_TZCNT              OFF CACHE BOOL "" FORCE)
set(USE_F16C               OFF CACHE BOOL "" FORCE)
set(USE_FMADD              OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK             OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS            ON  CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED       OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/joltphysics/Build)
# Belt-and-suspenders: Jolt's own CMakeLists.txt overrides CMAKE_CXX_FLAGS_DEBUG/RELEASE
# (see extern/joltphysics/Build/CMakeLists.txt ~line 191) inside its own directory scope
# with text containing no /MD or /MDd substring, and neither the CACHE variable above nor
# an explicit set_property(MSVC_RUNTIME_LIBRARY) on the Jolt target after the fact changed
# the generated Debug ItemDefinitionGroup at all (verified empirically - both left it with
# no <RuntimeLibrary> element, silently falling back to cl.exe's own /MD default even in
# Debug). Force the actual compiler flag directly instead - last /M* flag on the command
# line wins, so this overrides whatever came before it regardless of policy/property state.
target_compile_options(Jolt PRIVATE $<$<CONFIG:Debug>:/MDd> $<$<NOT:$<CONFIG:Debug>>:/MD>)

# Create the DLL
add_library(kraken SHARED
    source/entry.cpp
    source/config.cpp
    source/fix/physic.cpp
    source/fix/autobrakefix.cpp
    source/fix/objcontupgrade.cpp
    source/fix/luabinds.cpp
    source/fix/posteffectreload.cpp
    source/fix/fileserver.cpp
    source/fix/luabinds.cpp
    source/fix/wareuse.cpp
    source/fix/recollectionfix.cpp
    source/fix/ultrawide.cpp
    source/fix/fastloading.cpp
    source/fix/kineticfriction.cpp
    source/fix/cardan.cpp
    source/fix/tactics.cpp
    source/fix/complexschwarz.cpp
    source/fix/skinfix.cpp
    source/fix/cctlleakfix.cpp
    source/fix/locationdebug.cpp
    source/fix/difficultywndescapefix.cpp
    source/fix/mortarvolleylauncherfix.cpp
    source/fix/gunlights.cpp
    source/fix/bossmetalarm.cpp
    source/fix/testharness.cpp
    source/fix/jolt.cpp
    source/fix/joltshadow.cpp

    source/ext/logger.cpp
    source/ext/impulse.cpp
    source/ext/runtime.cpp
)

# Include directories
target_include_directories(kraken PRIVATE include)

# Windows-specific settings
if(MSVC)
    # Disable specific warnings if needed
    target_compile_options(kraken PRIVATE
        /W3
        /arch:AVX2    # AVX2 поддержка
        /fp:fast      # Быстрая float арифметика
        /MP
    )

    # Set runtime library
    set_property(TARGET kraken PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    # MinGW settings
    target_compile_options(kraken PRIVATE -Wall -Wextra)
endif()

# Define WIN32 for compatibility
target_compile_definitions(kraken PRIVATE WIN32_LEAN_AND_MEAN)

# Link Windows libraries
target_link_libraries(kraken PRIVATE
    kernel32
    user32
    lim
    hta
    Jolt
)

# Set output name
set_target_properties(kraken PROPERTIES
    OUTPUT_NAME "kraken"
    PREFIX ""
)

# Installation rules (optional)
install(TARGETS kraken
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)