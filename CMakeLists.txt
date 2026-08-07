cmake_minimum_required(VERSION 3.10)
project(kraken VERSION 0.1.1 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform check
if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only (Win32)")
endif()

# lim enables GLM fast-math. GLM's strict /Za mode is incompatible with
# /fp:fast on current MSVC, so keep the language extensions enabled.
set(GLM_ENABLE_LANG_EXTENSIONS ON CACHE BOOL "" FORCE)

add_subdirectory(extern/lim)
add_subdirectory(extern/hta)
add_subdirectory(extern/enet)

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

    source/ext/logger.cpp
    source/ext/impulse.cpp
    source/ext/runtime.cpp

    source/net/enet_transport.cpp
    source/net/session.cpp
    source/net/vehicle_snapshot.cpp
    source/net/runtime.cpp
)

# Include directories
target_include_directories(kraken PRIVATE
    include
    extern/enet/include
)

target_compile_definitions(kraken PRIVATE KRAKEN_NET_ENABLE_ENET=1)

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
    enet
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

include(CTest)
if(BUILD_TESTING)
    add_executable(kraken_net_loopback_test
        tests/net_loopback.cpp
        source/net/enet_transport.cpp
        source/net/session.cpp
    )
    target_compile_features(kraken_net_loopback_test PRIVATE cxx_std_20)
    target_compile_definitions(kraken_net_loopback_test PRIVATE
        WIN32_LEAN_AND_MEAN
        KRAKEN_NET_ENABLE_ENET=1
    )
    target_include_directories(kraken_net_loopback_test PRIVATE
        include
        extern/enet/include
    )
    target_link_libraries(kraken_net_loopback_test PRIVATE enet)
    set_property(TARGET kraken_net_loopback_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_loopback COMMAND kraken_net_loopback_test)

    add_executable(kraken_net_peer_test
        tests/net_peer.cpp
        source/net/enet_transport.cpp
        source/net/session.cpp
    )
    target_compile_features(kraken_net_peer_test PRIVATE cxx_std_20)
    target_compile_definitions(kraken_net_peer_test PRIVATE
        WIN32_LEAN_AND_MEAN
        KRAKEN_NET_ENABLE_ENET=1
    )
    target_include_directories(kraken_net_peer_test PRIVATE
        include
        extern/enet/include
    )
    target_link_libraries(kraken_net_peer_test PRIVATE enet)
    set_property(TARGET kraken_net_peer_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    add_executable(kraken_net_vehicle_snapshot_test
        tests/net_vehicle_snapshot.cpp
        source/net/vehicle_snapshot.cpp
    )
    target_compile_features(kraken_net_vehicle_snapshot_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_vehicle_snapshot_test PRIVATE include)
    set_property(TARGET kraken_net_vehicle_snapshot_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_vehicle_snapshot
        COMMAND kraken_net_vehicle_snapshot_test)
endif()
