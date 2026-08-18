cmake_minimum_required(VERSION 3.10)
project(kraken VERSION 0.1.1 LANGUAGES CXX)

# The Visual Studio generator's ZERO_CHECK invokes a nested CMake build for
# every target.  This project contains ENet and is built through the MCP
# harness, so regenerate explicitly through that harness instead.
set(CMAKE_SUPPRESS_REGENERATION ON)

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
    source/net/entity_registry.cpp
    source/net/entity_protocol.cpp
    source/net/combat_presentation.cpp
    source/net/combat_runtime.cpp
    source/net/host_world_registry.cpp
    source/net/impact_damage.cpp
    source/net/loadout_protocol.cpp
    source/net/lan_discovery.cpp
    source/net/match_protocol.cpp
    source/net/match_session.cpp
    source/net/native_object_archive.cpp
    source/net/resource_fingerprint.cpp
    source/net/static_world_identity.cpp
    source/net/input_command.cpp
    source/net/loot_transaction.cpp
    source/net/world_loot.cpp
    source/net/world_observer.cpp
    source/net/world_authority.cpp
    source/net/weapon_command.cpp
    source/net/world_replication.cpp
    source/net/world_state_snapshot.cpp
    source/net/quest_state_projection.cpp
    source/net/session.cpp
    source/net/snapshot_interpolation.cpp
    source/net/vehicle_descriptor.cpp
    source/net/vehicle_archive_validation.cpp
    source/net/vehicle_transfer.cpp
    source/net/vehicle_snapshot.cpp
    source/net/world_mutation_applier.cpp
    source/net/world_mutation_protocol.cpp
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
    ws2_32
    iphlpapi
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

    add_executable(kraken_net_lan_discovery_test
        tests/net_lan_discovery.cpp
        source/net/lan_discovery.cpp
    )
    target_compile_features(kraken_net_lan_discovery_test PRIVATE cxx_std_20)
    target_compile_definitions(kraken_net_lan_discovery_test PRIVATE
        WIN32_LEAN_AND_MEAN
    )
    target_include_directories(kraken_net_lan_discovery_test PRIVATE include)
    target_link_libraries(kraken_net_lan_discovery_test PRIVATE ws2_32 iphlpapi)
    set_property(TARGET kraken_net_lan_discovery_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_lan_discovery COMMAND kraken_net_lan_discovery_test)

    add_executable(kraken_net_pause_policy_test
        tests/net_pause_policy.cpp
    )
    target_compile_features(kraken_net_pause_policy_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_pause_policy_test PRIVATE include)
    set_property(TARGET kraken_net_pause_policy_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_pause_policy COMMAND kraken_net_pause_policy_test)

    add_executable(kraken_net_world_authority_test
        tests/net_world_authority.cpp
        source/net/world_authority.cpp
    )
    target_compile_features(kraken_net_world_authority_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_authority_test PRIVATE include)
    set_property(TARGET kraken_net_world_authority_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_authority
        COMMAND kraken_net_world_authority_test)

    add_executable(kraken_net_peer_test
        tests/net_peer.cpp
        source/net/enet_transport.cpp
        source/net/session.cpp
        source/net/input_command.cpp
        source/net/loot_transaction.cpp
        source/net/weapon_command.cpp
        source/net/vehicle_snapshot.cpp
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

    add_executable(kraken_net_entity_registry_test
        tests/net_entity_registry.cpp
        source/net/entity_registry.cpp
    )
    target_compile_features(kraken_net_entity_registry_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_entity_registry_test PRIVATE include)
    set_property(TARGET kraken_net_entity_registry_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_entity_registry
        COMMAND kraken_net_entity_registry_test)

    add_executable(kraken_net_impact_damage_test
        tests/net_impact_damage.cpp
        source/net/impact_damage.cpp
    )
    target_compile_features(kraken_net_impact_damage_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_impact_damage_test PRIVATE include)
    set_property(TARGET kraken_net_impact_damage_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_impact_damage
        COMMAND kraken_net_impact_damage_test)

    add_executable(kraken_net_combat_presentation_test
        tests/net_combat_presentation.cpp
        source/net/combat_presentation.cpp
    )
    target_compile_features(kraken_net_combat_presentation_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_combat_presentation_test PRIVATE include)
    set_property(TARGET kraken_net_combat_presentation_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_combat_presentation
        COMMAND kraken_net_combat_presentation_test)

    add_executable(kraken_net_combat_runtime_test
        tests/net_combat_runtime.cpp
        source/net/combat_runtime.cpp
        source/net/combat_presentation.cpp
        source/net/native_object_archive.cpp
    )
    target_compile_features(kraken_net_combat_runtime_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_combat_runtime_test PRIVATE include)
    target_link_libraries(kraken_net_combat_runtime_test PRIVATE hta kernel32)
    target_compile_definitions(kraken_net_combat_runtime_test PRIVATE
        KRAKEN_RUNTIME_SOURCE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/source/net/runtime.cpp")
    set_property(TARGET kraken_net_combat_runtime_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_combat_runtime
        COMMAND kraken_net_combat_runtime_test)

    add_executable(kraken_net_entity_protocol_test
        tests/net_entity_protocol.cpp
        source/net/entity_protocol.cpp
        source/net/loadout_protocol.cpp
        source/net/vehicle_snapshot.cpp
    )
    target_compile_features(kraken_net_entity_protocol_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_entity_protocol_test PRIVATE include)
    set_property(TARGET kraken_net_entity_protocol_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_entity_protocol
        COMMAND kraken_net_entity_protocol_test)

    add_executable(kraken_net_loadout_protocol_test
        tests/net_loadout_protocol.cpp
        source/net/loadout_protocol.cpp
    )
    target_compile_features(kraken_net_loadout_protocol_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_loadout_protocol_test PRIVATE include)
    set_property(TARGET kraken_net_loadout_protocol_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_loadout_protocol
        COMMAND kraken_net_loadout_protocol_test)

    add_executable(kraken_net_snapshot_interpolation_test
        tests/net_snapshot_interpolation.cpp
        source/net/snapshot_interpolation.cpp
        source/net/vehicle_snapshot.cpp
    )
    target_compile_features(kraken_net_snapshot_interpolation_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_snapshot_interpolation_test PRIVATE include)
    set_property(TARGET kraken_net_snapshot_interpolation_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_snapshot_interpolation
        COMMAND kraken_net_snapshot_interpolation_test)

    add_executable(kraken_net_input_command_test
        tests/net_input_command.cpp
        source/net/input_command.cpp
    )
    target_compile_features(kraken_net_input_command_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_input_command_test PRIVATE include)
    set_property(TARGET kraken_net_input_command_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_input_command COMMAND kraken_net_input_command_test)

    add_executable(kraken_net_weapon_command_test
        tests/net_weapon_command.cpp
        source/net/weapon_command.cpp
    )
    target_compile_features(kraken_net_weapon_command_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_weapon_command_test PRIVATE include)
    set_property(TARGET kraken_net_weapon_command_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_weapon_command COMMAND kraken_net_weapon_command_test)

    add_executable(kraken_net_loot_transaction_test
        tests/net_loot_transaction.cpp
        source/net/loot_transaction.cpp
    )
    target_compile_features(kraken_net_loot_transaction_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_loot_transaction_test PRIVATE include)
    set_property(TARGET kraken_net_loot_transaction_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_loot_transaction COMMAND kraken_net_loot_transaction_test)

    add_executable(kraken_net_world_loot_test
        tests/net_world_loot.cpp
        source/net/world_loot.cpp
    )
    target_compile_features(kraken_net_world_loot_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_loot_test PRIVATE include)
    set_property(TARGET kraken_net_world_loot_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_loot COMMAND kraken_net_world_loot_test)

    add_executable(kraken_net_player_slots_test
        tests/net_player_slots.cpp
    )
    target_compile_features(kraken_net_player_slots_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_player_slots_test PRIVATE include)
    set_property(TARGET kraken_net_player_slots_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_player_slots COMMAND kraken_net_player_slots_test)

    add_executable(kraken_net_match_session_test
        tests/net_match_session.cpp
        source/net/match_session.cpp
    )
    target_compile_features(kraken_net_match_session_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_match_session_test PRIVATE include)
    set_property(TARGET kraken_net_match_session_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_match_session COMMAND kraken_net_match_session_test)

    add_executable(kraken_net_64_participant_test
        tests/net_64_participant.cpp
        source/net/entity_registry.cpp
        source/net/match_protocol.cpp
        source/net/match_session.cpp
        source/net/session.cpp
    )
    target_compile_features(kraken_net_64_participant_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_64_participant_test PRIVATE include)
    set_property(TARGET kraken_net_64_participant_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_64_participant
        COMMAND kraken_net_64_participant_test)

    add_executable(kraken_net_match_protocol_test
        tests/net_match_protocol.cpp
        source/net/match_protocol.cpp
    )
    target_compile_features(kraken_net_match_protocol_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_match_protocol_test PRIVATE include)
    set_property(TARGET kraken_net_match_protocol_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_match_protocol COMMAND kraken_net_match_protocol_test)

    add_executable(kraken_net_world_replication_test
        tests/net_world_replication.cpp
        source/net/world_replication.cpp
    )
    target_compile_features(kraken_net_world_replication_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_replication_test PRIVATE include)
    set_property(TARGET kraken_net_world_replication_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_replication
        COMMAND kraken_net_world_replication_test)

    add_executable(kraken_net_world_observer_test
        tests/net_world_observer.cpp
        source/net/world_observer.cpp
    )
    target_compile_features(kraken_net_world_observer_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_observer_test PRIVATE include)
    set_property(TARGET kraken_net_world_observer_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_observer
        COMMAND kraken_net_world_observer_test)

    add_executable(kraken_net_vehicle_descriptor_test
        tests/net_vehicle_descriptor.cpp
        source/net/vehicle_descriptor.cpp
        source/net/vehicle_archive_validation.cpp
    )
    target_compile_features(kraken_net_vehicle_descriptor_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_vehicle_descriptor_test PRIVATE include)
    set_property(TARGET kraken_net_vehicle_descriptor_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_vehicle_descriptor
        COMMAND kraken_net_vehicle_descriptor_test)

    add_executable(kraken_net_vehicle_transfer_test
        tests/net_vehicle_transfer.cpp
        source/net/vehicle_transfer.cpp
        source/net/vehicle_descriptor.cpp
        source/net/vehicle_archive_validation.cpp
    )
    target_compile_features(kraken_net_vehicle_transfer_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_vehicle_transfer_test PRIVATE include)
    set_property(TARGET kraken_net_vehicle_transfer_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_vehicle_transfer
        COMMAND kraken_net_vehicle_transfer_test)

    add_executable(kraken_net_world_mutation_protocol_test
        tests/net_world_mutation_protocol.cpp
        source/net/world_mutation_protocol.cpp
        source/net/world_observer.cpp
    )
    target_compile_features(kraken_net_world_mutation_protocol_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_mutation_protocol_test PRIVATE include)
    set_property(TARGET kraken_net_world_mutation_protocol_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_mutation_protocol
        COMMAND kraken_net_world_mutation_protocol_test)

    add_executable(kraken_net_resource_fingerprint_test
        tests/net_resource_fingerprint.cpp
        source/net/resource_fingerprint.cpp
    )
    target_compile_features(kraken_net_resource_fingerprint_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_resource_fingerprint_test PRIVATE include)
    set_property(TARGET kraken_net_resource_fingerprint_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_resource_fingerprint
        COMMAND kraken_net_resource_fingerprint_test)

    add_executable(kraken_net_static_world_identity_test
        tests/net_static_world_identity.cpp
        source/net/static_world_identity.cpp
    )
    target_compile_features(kraken_net_static_world_identity_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_static_world_identity_test PRIVATE include)
    set_property(TARGET kraken_net_static_world_identity_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_static_world_identity
        COMMAND kraken_net_static_world_identity_test)

    add_executable(kraken_net_world_state_snapshot_test
        tests/net_world_state_snapshot.cpp
        source/net/world_state_snapshot.cpp
        source/net/world_observer.cpp
        source/net/world_replication.cpp
    )
    target_compile_features(kraken_net_world_state_snapshot_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_state_snapshot_test PRIVATE include)
    set_property(TARGET kraken_net_world_state_snapshot_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_state_snapshot
        COMMAND kraken_net_world_state_snapshot_test)

    add_executable(kraken_net_quest_state_projection_test
        tests/net_quest_state_projection.cpp
        source/net/quest_state_projection.cpp
        source/net/static_world_identity.cpp
    )
    target_compile_features(kraken_net_quest_state_projection_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_quest_state_projection_test PRIVATE include)
    set_property(TARGET kraken_net_quest_state_projection_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_quest_state_projection
        COMMAND kraken_net_quest_state_projection_test)

    add_executable(kraken_net_world_mutation_applier_test
        tests/net_world_mutation_applier.cpp
        source/net/host_world_registry.cpp
        source/net/world_mutation_applier.cpp
        source/net/world_mutation_protocol.cpp
        source/net/world_observer.cpp
    )
    target_compile_features(kraken_net_world_mutation_applier_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_world_mutation_applier_test PRIVATE include)
    set_property(TARGET kraken_net_world_mutation_applier_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_world_mutation_applier
        COMMAND kraken_net_world_mutation_applier_test)

    add_executable(kraken_net_native_object_archive_test
        tests/net_native_object_archive.cpp
        source/net/native_object_archive.cpp
    )
    target_compile_features(kraken_net_native_object_archive_test PRIVATE cxx_std_20)
    target_include_directories(kraken_net_native_object_archive_test PRIVATE include)
    target_link_libraries(kraken_net_native_object_archive_test PRIVATE hta kernel32)
    set_property(TARGET kraken_net_native_object_archive_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME kraken_net_native_object_archive
        COMMAND kraken_net_native_object_archive_test)
endif()
