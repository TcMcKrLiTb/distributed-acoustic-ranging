cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME DistributedAcousticRanging)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM, C and C++ languages
enable_language(C CXX ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME}
        TouchGFX/target/CortexMMCUInstrumentation.cpp
        Drivers/BSP/Components/wm8994/wm8994.c
        Drivers/BSP/Components/ft5336/ft5336.c
        Drivers/STM32746G-Discovery/stm32746g_discovery.c
        Drivers/STM32746G-Discovery/stm32746g_discovery_audio.c
        Drivers/STM32746G-Discovery/stm32746g_discovery_ts.c
        Drivers/BSP/Components/lan8742/lan8742.c)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Add PTP code into project by use compiling macro
target_compile_definitions(stm32cubemx INTERFACE
    HAL_ETH_USE_PTP
)

# Add touchgfx generated sources and libraries
add_subdirectory(cmake/touchgfx)

# Add application sources
add_subdirectory(App)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths

        ${CMAKE_SOURCE_DIR}/Drivers/BSP/Components/ft5336
        ${CMAKE_SOURCE_DIR}/Drivers/BSP/Components/n25q128a
        ${CMAKE_SOURCE_DIR}/Drivers/BSP/Components/Common
        ${CMAKE_SOURCE_DIR}/Drivers/BSP/Components/wm8994
        ${CMAKE_SOURCE_DIR}/Drivers/BSP/Components/lan8742
        ${CMAKE_SOURCE_DIR}/Drivers/STM32746G-Discovery
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
