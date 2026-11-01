cmake_minimum_required(VERSION 3.21)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(BasuInputBuffer VERSION 2.9.0)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# iOS: Geode's generated bindings source uses FMOD types without including the FMOD header
# (fixed in the bindings repo later); force-include it so the iOS build compiles
if (("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS) AND TARGET GeodeBindings)
    target_compile_options(GeodeBindings PRIVATE -include Geode/fmod/fmod.hpp)
endif()

# iOS: the mod's dylib has to carry a code signature or some iOS launchers refuse to load it
# ("missing code signature"); sign it ad-hoc right after it's built, before it's packaged
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Signing the iOS dylib (ad-hoc)")
endif()

setup_geode_mod(${PROJECT_NAME})
