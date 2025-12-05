cmake_minimum_required(VERSION 3.16)
project(charmOS-kernel C ASM_NASM ASM-ATT)
set(CARGO_TARGET x86_64-unknown-none)
set(CMAKE_C_STANDARD 11)

file(GLOB_RECURSE C_SOURCES *.c)
file(GLOB_RECURSE ASM_SOURCES *.S)
file(GLOB_RECURSE NASM_SOURCES *.asm)

enable_language(ASM_NASM)
enable_language(ASM-ATT)

set_source_files_properties(${NASM_SOURCES} PROPERTIES LANGUAGE ASM_NASM)
set_source_files_properties(${ASM_SOURCES} PROPERTIES LANGUAGE ASM-ATT)

include_directories(.)
include(uACPI/uacpi.cmake)

set(EARLY_INIT_FILES
    ${CMAKE_SOURCE_DIR}/kernel/main.c
    ${CMAKE_SOURCE_DIR}/kernel/smp/mp.c
    ${CMAKE_SOURCE_DIR}/kernel/bootstage.c
    ${CMAKE_SOURCE_DIR}/kernel/misc/string.c
    ${CMAKE_SOURCE_DIR}/kernel/console/printf.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/asan.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/buddy/init.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/bitmap.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/vmm.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/buddy/core.c
    ${CMAKE_SOURCE_DIR}/kernel/sch/irql.c
)

add_executable(kernel ${C_SOURCES} ${ASM_SOURCES} ${NASM_SOURCES})

set(SYMS_SOURCE "${CMAKE_SOURCE_DIR}/syms/syms_dummy.c")
if(EXISTS "${CMAKE_SOURCE_DIR}/syms/fullsyms.c")
    set(SYMS_SOURCE "${CMAKE_SOURCE_DIR}/syms/fullsyms.c")
endif()

target_sources(
    kernel
    PRIVATE
    ${SYMS_SOURCE}
    ${UACPI_SOURCES}
)

target_include_directories(
    kernel
    PRIVATE
    ${UACPI_INCLUDES}
)
