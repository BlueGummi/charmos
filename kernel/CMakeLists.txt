cmake_minimum_required(VERSION 3.16)
project(charmOS-kernel C ASM_NASM ASM-ATT)

set(CARGO_TARGET x86_64-unknown-none)
set(CMAKE_C_STANDARD 11)

file(GLOB_RECURSE C_SOURCES    CONFIGURE_DEPENDS *.c)

foreach(FILE_PATH ${C_SOURCES})
    if(FILE_PATH MATCHES "syms/")
        list(REMOVE_ITEM C_SOURCES ${FILE_PATH})
    endif()
endforeach()

file(GLOB_RECURSE ASM_SOURCES  CONFIGURE_DEPENDS *.S)
file(GLOB_RECURSE NASM_SOURCES CONFIGURE_DEPENDS *.asm)

enable_language(ASM_NASM)
enable_language(ASM-ATT)

set_source_files_properties(${NASM_SOURCES} PROPERTIES LANGUAGE ASM_NASM)
set_source_files_properties(${ASM_SOURCES}  PROPERTIES LANGUAGE ASM-ATT)

include_directories(.)
include(uACPI/uacpi.cmake)

set(EARLY_INIT_FILES
    ${CMAKE_SOURCE_DIR}/kernel/main.c
    ${CMAKE_SOURCE_DIR}/kernel/smp/smp.c
    ${CMAKE_SOURCE_DIR}/kernel/bootstage.c
    ${CMAKE_SOURCE_DIR}/kernel/string.c
    ${CMAKE_SOURCE_DIR}/kernel/console/printf.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/asan.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/pmm.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/buddy/init.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/bitmap.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/vmm.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/buddy/core.c
    ${CMAKE_SOURCE_DIR}/kernel/sch/irql.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/page_alloc.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/slab/slab.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/slab/percpu.c
    ${CMAKE_SOURCE_DIR}/kernel/mem/slab/free_queue.c
)

if(EXISTS "${CMAKE_SOURCE_DIR}/kernel/syms/fullsyms.c")
    set(SYMS_SOURCE "${CMAKE_SOURCE_DIR}/kernel/syms/fullsyms.c")
    message(STATUS "Symbol source: fullsyms.c (resolved)")
else()
    set(SYMS_SOURCE "${CMAKE_SOURCE_DIR}/kernel/syms/syms_dummy.c")
    message(STATUS "Symbol source: syms_dummy.c (run `make regen-syms` after first build)")
endif()

add_executable(kernel ${C_SOURCES} ${ASM_SOURCES} ${NASM_SOURCES})

if(DEBUG_ASAN)
    set_source_files_properties(${EARLY_INIT_FILES} ${UACPI_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=kernel-address")
endif()

target_sources(kernel
    PRIVATE
        ${SYMS_SOURCE}
        ${UACPI_SOURCES}
)

target_include_directories(kernel
    PRIVATE
        ${UACPI_INCLUDES}
)
