cmake_minimum_required(VERSION 3.20)

project(kraken VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only (Win32)")
endif()

add_library(kraken SHARED)
add_subdirectory(entry)
add_subdirectory(common)
add_subdirectory(config)
add_subdirectory(fixes)
add_subdirectory(impulse)
add_subdirectory(logger)
add_subdirectory(memory)
add_subdirectory(runtime)

target_include_directories(kraken PRIVATE .)

if (MSVC)
    target_compile_options(kraken PRIVATE /W3)
    target_compile_options(kraken PRIVATE /arch:AVX2)
    target_compile_options(kraken PRIVATE /fp:fast)
    target_compile_options(kraken PRIVATE /MP)
endif()

target_compile_definitions(kraken PRIVATE WIN32_LEAN_AND_MEAN)

target_link_libraries(kraken PRIVATE kernel32)
target_link_libraries(kraken PRIVATE user32)
target_link_libraries(kraken PRIVATE lim)
target_link_libraries(kraken PRIVATE hta)

set_target_properties(kraken PROPERTIES
    OUTPUT_NAME "kraken"
    PREFIX ""
)

install(TARGETS kraken
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)