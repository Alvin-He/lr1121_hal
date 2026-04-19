cmake_minimum_required(VERSION 3.22.1)

project(lr1121_hal
    VERSION 0.0.0
    LANGUAGES C CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_FLAGS "-Wall -Wextra -Wpedantic")
#
# Dependencies
# 

# SWDR001
add_subdirectory("${CMAKE_SOURCE_DIR}/deps/SWDR001")

# libgpiod
add_library(libgpiod STATIC IMPORTED)
set_target_properties(libgpiod PROPERTIES 
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/deps/lib/libgpiod.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/include"
)

# openssl
find_package(OpenSSL COMPONENTS Crypto)

find_library(LGPIO_LIB NAMES lgpio REQUIRED)

#
# Targets
#

add_library(lr11xx_hal STATIC
    src/lr11xx_linux_hal.c
)
add_library(lr11xx_hal::lr11xx_hal ALIAS lr11xx_hal)
target_link_libraries(lr11xx_hal 
    PUBLIC lr11xx_driver::lr11xx_driver
    PRIVATE ${LGPIO_LIB}
)

if(OpenSSL_FOUND)
    add_executable(lr11xx_firmware_updater src/firmware_updater.c)
    target_link_libraries(lr11xx_firmware_updater
        PUBLIC lr11xx_hal
        PRIVATE OpenSSL::Crypto
    )
else()
    message(AUTHOR_WARNING "No OpenSSL found, unable to compile lr11xx_firmware_updater")
endif()
