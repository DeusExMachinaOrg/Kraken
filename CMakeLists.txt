cmake_minimum_required(VERSION 3.10)
project(kraken VERSION 0.1.1 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform check
if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only (Win32)")
endif()

# Find DirectX SDK
if(DEFINED ENV{DXSDK_DIR})
    set(DXSDK_DIR $ENV{DXSDK_DIR})
    message(STATUS "Found DirectX SDK: ${DXSDK_DIR}")
else()
    message(WARNING "DXSDK_DIR environment variable not set. Trying default paths...")
    # Try common installation paths
    if(EXISTS "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)")
        set(DXSDK_DIR "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)")
    elseif(EXISTS "C:/DXSDK")
        set(DXSDK_DIR "C:/DXSDK")
    else()
        message(FATAL_ERROR "Could not find DirectX SDK. Please set DXSDK_DIR environment variable.")
    endif()
endif()

# Set DirectX paths
set(DXSDK_INCLUDE_DIR "${DXSDK_DIR}/Include")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(DXSDK_LIB_DIR "${DXSDK_DIR}/Lib/x64")
else()
    set(DXSDK_LIB_DIR "${DXSDK_DIR}/Lib/x86")
endif()

# Verify DirectX paths exist
if(NOT EXISTS "${DXSDK_INCLUDE_DIR}")
    message(FATAL_ERROR "DirectX SDK include directory not found: ${DXSDK_INCLUDE_DIR}")
endif()
if(NOT EXISTS "${DXSDK_LIB_DIR}")
    message(FATAL_ERROR "DirectX SDK library directory not found: ${DXSDK_LIB_DIR}")
endif()

add_subdirectory(extern/hta)

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
    source/fix/render.cpp

    source/hta/m3d/ui/GarageWnd.cpp

    source/render/native/Image.cpp
    source/render/native/Debug.cpp
    source/render/native/Uniform.cpp
    source/render/CDevice.cpp

    source/ext/logger.cpp
    source/ext/impulse.cpp
    source/ext/runtime.cpp
    source/stdafx.cpp
)

# Include directories
target_include_directories(kraken PRIVATE
    include
    source
    ${DXSDK_INCLUDE_DIR}
)

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

# Link DirectX libraries
target_link_directories(kraken PRIVATE ${DXSDK_LIB_DIR})

# Link Windows and DirectX libraries
target_link_libraries(kraken PRIVATE
    kernel32
    user32
    d3d9
    d3dx9
    hta
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