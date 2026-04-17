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

add_library(lr11xx_hal STATIC
    src/lr11xx_hal.c
)

#
# Targets
#
add_library(lr11xx_hal::lr11xx_hal ALIAS lr11xx_hal)
target_link_libraries(lr11xx_hal 
    PUBLIC lr11xx_driver::lr11xx_driver
    PRIVATE libgpiod
)