cmake_minimum_required(VERSION 3.20)

project(spp LANGUAGES CXX)

set(SPP_SOURCES
    src/core/unify.cpp
)

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

if(WIN32)
    list(APPEND SPP_SOURCES src/platform/w32/unify.cpp)
elseif(LINUX OR APPLE)
    list(APPEND SPP_SOURCES src/platform/pos/unify.cpp)
else()
    message(FATAL_ERROR "Unsupported OS: only Windows, Linux, and macOS are supported.")
endif()

add_library(spp STATIC ${SPP_SOURCES})
target_include_directories(spp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(spp PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF LINKER_LANGUAGE CXX)

target_compile_definitions(spp PUBLIC
    $<$<CONFIG:Debug>:SPP_DEBUG_BUILD>
    $<$<CONFIG:RelWithDebInfo>:SPP_RELWITHDEBINFO_BUILD>
    $<$<CONFIG:Release>:SPP_RELEASE_BUILD>
    $<$<CONFIG:MinSizeRel>:SPP_RELEASE_BUILD>
)

if(WIN32)
    target_link_libraries(spp PRIVATE ws2_32 Synchronization)
endif()

if(MSVC)
    if(MSVC_VERSION LESS 1939)
        message(FATAL_ERROR "Unsupported MSVC version: only 19.39+ is supported.")
    endif()

    string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    target_compile_definitions(spp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _HAS_EXCEPTIONS=0)
    target_link_options(spp PRIVATE /NODEFAULTLIB:MSVCRT)
    target_compile_options(spp PRIVATE /MP /W4 /GR- /GS- /EHa- /wd4201)
else()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        message(FATAL_ERROR "Unsupported compiler: only MSVC and Clang are supported.")
    endif()
    target_compile_options(spp PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()
