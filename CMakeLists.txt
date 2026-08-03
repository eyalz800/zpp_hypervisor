cmake_minimum_required(VERSION 3.25)
project(zpp_hypervisor_project LANGUAGES NONE)

include(ExternalProject)

set(ZPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
string(TOLOWER "${CMAKE_BUILD_TYPE}" ZPP_CONFIG_LOWER)
set(ZPP_OUTPUT_DIR "${ZPP_SOURCE_DIR}/out/${ZPP_CONFIG_LOWER}/x86_64")

set(FREESTANDING_TOOLCHAIN "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-x86_64-freestanding.cmake")

# === Hypervisor (freestanding x86_64 PIE ELF) ===
ExternalProject_Add(hypervisor
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hypervisor"
    BINARY_DIR "${CMAKE_BINARY_DIR}/hypervisor"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${FREESTANDING_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
)
