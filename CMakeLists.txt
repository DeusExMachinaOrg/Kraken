cmake_minimum_required(VERSION 3.10)
project(kraken VERSION 0.1.1 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform check
if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only (Win32)")
endif()

# Source files
set(SOURCES
    source/entry.cpp
    source/config.cpp
    source/fix/physic.cpp
    source/fix/autobrakefix.cpp
    source/fix/objcontupgrade.cpp
    source/fix/fileserver.cpp
    source/fix/luabinds.cpp
)

# Header files
set(HEADERS
    include/stdafx.hpp
    include/bridge.hpp
    include/config.hpp
    include/routines.hpp
    include/utils.hpp
    include/fix/physic.hpp
    include/fix/autobrakefix.hpp
    include/fix/objcontupgrade.hpp
    include/fix/luabinds.hpp
    include/hta/pointers.hpp
    include/hta/CStr.h
    include/hta/CVector.h
    include/hta/Func.h
    include/hta/Game.h
    include/hta/Quaternion.h
    include/hta/ai/ActionType.h
    include/hta/ai/AIParam.h
    include/hta/ai/AIParam.h
    include/hta/ai/Basket.h
    include/hta/ai/Cabin.h
    include/hta/ai/Chassis.h
    include/hta/ai/CollisionInfo.h
    include/hta/ai/ComplexPhysicObj.h
    include/hta/ai/ComplexPhysicObjPartDescription.h
    include/hta/ai/ComplexPhysicObjPrototypeInfo.h
    include/hta/ai/Component.h
    include/hta/ai/DecalData.h
    include/hta/ai/dMass.h
    include/hta/ai/Geom.h
    include/hta/ai/GeometryInfo.h
    include/hta/ai/GeomRepository.h
    include/hta/ai/GeomRepositoryItem.h
    include/hta/ai/GeomTransform.h
    include/hta/ai/IzvratRepository.h
    include/hta/ai/Modifier.h
    include/hta/ai/Numeric.h
    include/hta/ai/NumericInRange.h
    include/hta/ai/NumericInRangeRegenerating.h
    include/hta/ai/Obj.h
    include/hta/ai/Path.h
    include/hta/ai/PhysicBody.h
    include/hta/ai/PhysicObj.h
    include/hta/ai/PhysicObjPrototypeInfo.h
    include/hta/ai/PrototypeInfo.h
    include/hta/ai/SgNode.h
    include/hta/ai/SimplePhysicObj.h
    include/hta/ai/SimplePhysicObjPrototypeInfo.h
    include/hta/ai/Vehicle.h
    include/hta/ai/VehiclePart.h
    include/hta/ai/FlatLine.h
    include/hta/ai/VehiclePrototypeInfo.h
    include/hta/ai/DrivingValues.h
    include/hta/m3d/Object.h
    include/hta/m3d/RefCountedBase.h
    include/hta/m3d/Application.h
    include/hta/m3d/CMiracle3d.h
    include/hta/m3d/IRenderer.h
    include/hta/m3d/GameImpulse.h
    include/hta/m3d/Kernel.h
    include/hta/m3d/TruxxUiManager.h
    include/hta/m3d/WeaponGroupManager.h

    include/ext/runtime.hpp
    source/ext/runtime.cpp
)

# Resource files
set(RESOURCES
    source/meta.rc
)

# Create the DLL
add_library(kraken SHARED ${SOURCES} ${HEADERS} ${RESOURCES})

# Include directories
target_include_directories(kraken PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Windows-specific settings
if(MSVC)
    # Disable specific warnings if needed
    target_compile_options(kraken PRIVATE /W3)
    
    # Set runtime library
    set_property(TARGET kraken PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    # MinGW settings
    target_compile_options(kraken PRIVATE -Wall -Wextra)
endif()

# Define WIN32 for compatibility
target_compile_definitions(kraken PRIVATE WIN32_LEAN_AND_MEAN)

# Link Windows libraries
target_link_libraries(kraken PRIVATE kernel32 user32)

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