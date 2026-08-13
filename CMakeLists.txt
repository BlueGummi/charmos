cmake_minimum_required(VERSION 3.16)
project(charmOS C ASM_NASM)

set(CMAKE_C_STANDARD_REQUIRED ON)
set(ARCH "x86_64")
set(IMAGE_NAME "charmos-${ARCH}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        "Debug"
        CACHE STRING "Build type: Debug, Release, RelWithDebInfo, MinSizeRel" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif ()

message(STATUS "charmOS build type: ${CMAKE_BUILD_TYPE}")

include(features) # compile-time feature toggles
include(flags) # compiler / linker / codegen

add_subdirectory(kernel)
target_include_directories(kernel PRIVATE include)

include(emulation)

add_custom_target(
    clean-full
    COMMAND ${CMAKE_COMMAND} -E rm -rf iso_root ${IMAGE_NAME}.iso ${IMAGE_NAME}.hdd kernel-deps limine ovmf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "")
message(STATUS "charmOS configuration:")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Arch         : ${ARCH}")
message(STATUS "  Stack usage  : ${KERNEL_STACK_USAGE}")
message(STATUS "  Tests        : ${TEST_ENABLED} (TEST_ALL=${TEST_ALL})")
message(STATUS "  Profiling    : ${PROFILING_ENABLED} (PROFILING_ALL=${PROFILING_ALL})")
message(STATUS "  QEMU KVM     : ${QEMU_KVM}")
message(STATUS "  QEMU IOMMU   : ${QEMU_IOMMU}")
message(STATUS "  QEMU NUMA    : ${QEMU_NUMA}")
message(STATUS "  QEMU USB     : ${QEMU_USB}")
message(STATUS "  QEMU NVMe    : ${QEMU_NVME}")
message(STATUS "  QEMU memory  : ${QEMU_MEM_SIZE}")
message(STATUS "  QEMU SMP     : ${QEMU_SMP_TOPO}")
message(STATUS "")
