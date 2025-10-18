cmake_minimum_required(VERSION 3.14)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake)
project(RampAgent VERSION "0.0.1")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# Find external dependencies
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Source files
set(SOURCES
    src/RampAgent.cpp
)

# Define the plugin library (create target before using target_*
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# Imported SDK lib
add_library(EUROSCOPE_SDK STATIC IMPORTED)
set_target_properties(EUROSCOPE_SDK PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/External/EuroScopeSDK/lib/EuroScopePlugInDll.lib"
)

# Target includes and definitions
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/External/httplib
    ${CMAKE_SOURCE_DIR}/External/EuroScopeSDK/include
)
target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# Link dependencies
target_link_libraries(${PROJECT_NAME} PRIVATE 
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    EUROSCOPE_SDK
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
