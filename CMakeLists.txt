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
    include/hta/ai/Wheel.cpp
    include/hta/ai/Vehicle.cpp
    include/hta/ai/CServer.cpp
    include/hta/ai/DynamicScene.cpp
    include/hta/ai/AI.cpp
    include/hta/ai/Obj.cpp
    include/hta/ai/ObjContainer.cpp
    include/hta/ai/StaticAutoGun.cpp
    include/hta/m3d/RoadNode.cpp
    include/hta/m3d/CWorld.cpp
    include/hta/m3d/WeatherManager.cpp
    include/hta/m3d/Landscape.cpp
    include/hta/m3d/WheelTraceMgr.cpp
    include/hta/m3d/AIParam.cpp

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
)

# Header files
set(HEADERS
    include/stdafx.hpp
    include/bridge.hpp
    include/config.hpp
    include/configstructs.hpp
    include/routines.hpp
    include/utils.hpp
    include/fix/physic.hpp
    include/fix/autobrakefix.hpp
    include/fix/objcontupgrade.hpp
    include/fix/luabinds.hpp
    include/fix/posteffectreload.hpp
    include/fix/ultrawide.hpp
    include/fix/fastloading.hpp
    include/fix/cardan.hpp
    include/fix/tactics.hpp
    include/fix/complexschwarz.hpp
    include/fix/skinfix.hpp
    include/hta/pointers.hpp
    include/hta/CStr.h
    include/hta/CVector.h
    include/hta/Func.h
    include/hta/Game.h
    include/hta/Quaternion.h
    include/hta/DragDropItemsWnd.h
    include/hta/Enums.hpp
    include/hta/ai/ActionType.h
    include/hta/ai/BaseFunction.hpp
    include/hta/ai/Basket.h
    include/hta/ai/Cabin.h
    include/hta/ai/Chassis.h
    include/hta/ai/CollisionInfo.h
    include/hta/ai/ComplexPhysicObj.h
    include/hta/ai/ComplexPhysicObjPartDescription.h
    include/hta/ai/Component.h
    include/hta/ai/CompoundGun.hpp
    include/hta/ai/CompoundVehiclePart.hpp
    include/hta/ai/DamageInfo.hpp
    include/hta/ai/DecalData.h
    include/hta/ai/DynamicQuestPeace.hpp
    include/hta/ai/dMass.h
    include/hta/ai/FuncPtr.hpp
    include/hta/ai/Gadget.hpp
    include/hta/ai/Geom.h
    include/hta/ai/GeometryInfo.h
    include/hta/ai/GeomRepository.h
    include/hta/ai/GeomRepositoryItem.h
    include/hta/ai/GeomTransform.h
    include/hta/ai/GlobalProperties.h
    include/hta/ai/Gun.hpp
    include/hta/ai/IPriceCoeffProvider.h
    include/hta/ai/IzvratRepository.h
    include/hta/ai/Modifier.h
    include/hta/ai/Numeric.h
    include/hta/ai/NumericInRange.h
    include/hta/ai/NumericInRangeRegenerating.h
    include/hta/ai/Obj.hpp
    include/hta/ai/Path.h
    include/hta/ai/PhysicBody.h
    include/hta/ai/PhysicObj.h
    include/hta/ai/PrototypeInfo.h
    include/hta/ai/SimplePhysicObj.h
    include/hta/ai/SimplePhysicObjPrototypeInfo.h
    include/hta/ai/VehiclePart.h
    include/hta/ai/VehicleRecollection.h
    include/hta/ai/FlatLine.h
    include/hta/ai/VehiclePrototypeInfo.h
    include/hta/ai/DrivingValues.h
    include/hta/ai/PrototypeManager.h
    include/hta/ai/Player.hpp
    include/hta/ai/ObjContainer.hpp
    include/hta/ai/StaticAutoGun.hpp
    include/hta/m3d/Object.h
    include/hta/m3d/RefCountedBase.h
    include/hta/m3d/Application.h
    include/hta/m3d/CMiracle3d.h
    include/hta/m3d/IRenderer.h
    include/hta/m3d/GameImpulse.h
    include/hta/m3d/Kernel.h
    include/hta/m3d/TruxxUiManager.h
    include/hta/m3d/WeaponGroupManager.h
    include/hta/m3d/WeatherManager.h
    include/hta/m3d/CClient.h
    include/hta/m3d/AnimInfo.hpp

    include/ext/logger.hpp
    source/ext/logger.cpp

    include/ext/impulse.hpp
    source/ext/impulse.cpp

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
    target_compile_options(kraken PRIVATE 
        /W3
        /arch:AVX2    # AVX2 поддержка
        /fp:fast      # Быстрая float арифметика
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