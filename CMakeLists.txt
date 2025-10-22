cmake_minimum_required(VERSION 3.14)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake)
SET(VCPKG_TARGET_TRIPLET "x86-windows-static" CACHE STRING "")
project(RampAgent VERSION "1.0.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Use static runtime libraries
SET(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

IF (MSVC)
    IF (CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        STRING(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    ELSE ()
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    ENDIF ()
    IF (NOT CMAKE_CXX_FLAGS MATCHES "/MP")
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
        SET(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} /MP")
    ENDIF ()

    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /sdl /permissive- /DNOMINMAX")
    SET(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} /sdl /permissive- /DNOMINMAX")
    SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MANIFESTUAC:NO /ignore:4099")
    ADD_DEFINITIONS(/D_USRDLL)

    FOREACH(flag_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE)
        IF(${flag_var} MATCHES "/MD")
            STRING(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        ENDIF()
    ENDFOREACH()
ENDIF ()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_compile_definitions(DIR_SEPARATOR="\\\\")
else()
    add_compile_definitions(DIR_SEPARATOR="/")
endif()

if(DEV)
    message(STATUS "Development mode enabled")
    add_compile_definitions(DEV=1)
endif()

configure_file(
    ${CMAKE_SOURCE_DIR}/src/Version.h.in
    ${CMAKE_BINARY_DIR}/Version.h
)

# set DEBUG mode
if (DEBUG)
    add_compile_definitions(DEBUG=1)
    message(STATUS "DEBUG mode enabled")
endif()

# Add SDK include directory
include_directories(
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/src
)


# Source files
set(SOURCES
    src/RampAgent.cpp
)

# Define the plugin library (create target before using target_*
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# Find external dependencies
FIND_PACKAGE(nlohmann_json CONFIG REQUIRED)
FIND_PACKAGE(OpenSSL CONFIG REQUIRED)

ADD_DEFINITIONS(
    -D_CRT_SECURE_NO_WARNINGS
    -DWIN32_LEAN_AND_MEAN
)

# --- EuroScope SDK Prebuilt Library ---
set(EUROSCOPE_LIB "${CMAKE_SOURCE_DIR}/External/EuroScopeSDK/lib/EuroScopePlugInDLL.lib")

if(NOT EXISTS "${EUROSCOPE_LIB}")
    message(FATAL_ERROR "EuroScopePlugInDLL.lib not found at ${EUROSCOPE_LIB}")
endif()

add_library(EuroScopePluginDLL STATIC IMPORTED)
set_target_properties(EuroScopePluginDLL PROPERTIES
    IMPORTED_LOCATION "${EUROSCOPE_LIB}"
)

target_include_directories(EuroScopePluginDLL INTERFACE
    "${CMAKE_SOURCE_DIR}/External/EuroScopeSDK/include"
)



# Target includes and definitions
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/External/httplib
    ${CMAKE_SOURCE_DIR}/External/EuroScopeSDK/include
)
target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)


target_include_directories(RampAgent PRIVATE ${CMAKE_SOURCE_DIR}/External/httplib)


TARGET_LINK_LIBRARIES(${PROJECT_NAME} PRIVATE
    EuroScopePluginDLL
    OpenSSL::SSL OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    crypt32.lib ws2_32.lib Shlwapi.lib
)

# Output dirs and properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PREFIX ""
)

# Apple-specific output name
if(${CMAKE_HOST_APPLE})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME ${PROJECT_NAME}-${CMAKE_HOST_SYSTEM_PROCESSOR}
    )
endif()
