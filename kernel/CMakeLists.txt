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
    ${CMAKE_SOURCE_DIR}/kernel/console/crash.c
    ${CMAKE_SOURCE_DIR}/kernel/console/panic.c
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

set(SYMS_SOURCE "${CMAKE_SOURCE_DIR}/kernel/syms/syms_reserve.c")

# We need to make sure the versions are right on macOS
find_program(KERNEL_NM      NAMES x86_64-linux-gnu-nm      x86_64-elf-nm      llvm-nm      llvm-nm-19 llvm-nm-18 llvm-nm-17 llvm-nm-16      nm      PATH_SUFFIXES llvm-19/bin llvm-18/bin llvm-17/bin llvm-16/bin REQUIRED)
find_program(KERNEL_OBJCOPY NAMES x86_64-linux-gnu-objcopy x86_64-elf-objcopy llvm-objcopy llvm-objcopy-19 llvm-objcopy-18 llvm-objcopy-17 llvm-objcopy-16 objcopy PATH_SUFFIXES llvm-19/bin llvm-18/bin llvm-17/bin llvm-16/bin REQUIRED)

find_program(KERNEL_OBJDUMP NAMES x86_64-linux-gnu-objdump x86_64-elf-objdump llvm-objdump llvm-objdump-19 llvm-objdump-18 llvm-objdump-17 llvm-objdump-16 objdump PATH_SUFFIXES llvm-19/bin llvm-18/bin llvm-17/bin llvm-16/bin)
if(NOT KERNEL_OBJDUMP)
    set(KERNEL_OBJDUMP "objdump")
    message(WARNING
        "no objdump found: panic backtraces will have no file:line")
endif()

set(SYMS_HEADER "${CMAKE_SOURCE_DIR}/include/linker/symbol_table.h")
file(STRINGS "${SYMS_HEADER}" SYMS_RESERVE_LINE
     REGEX "^#define[ \t]+KERNEL_SYMS_RESERVE[ \t]+[0-9]+")
if(NOT SYMS_RESERVE_LINE)
    message(FATAL_ERROR "could not read KERNEL_SYMS_RESERVE from ${SYMS_HEADER}")
endif()
string(REGEX REPLACE "^#define[ \t]+KERNEL_SYMS_RESERVE[ \t]+([0-9]+).*" "\\1"
       KERNEL_SYMS_RESERVE "${SYMS_RESERVE_LINE}")

message(STATUS "Symbol table  : stamped post-link, ${KERNEL_SYMS_RESERVE} bytes reserved")

add_executable(kernel ${C_SOURCES} ${ASM_SOURCES} ${NASM_SOURCES})

if(DEBUG_ASAN)
    file(GLOB_RECURSE FLANTERM_SOURCES CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/kernel/flanterm/*.c)

    set_source_files_properties(${EARLY_INIT_FILES} ${UACPI_SOURCES}
        ${FLANTERM_SOURCES}
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

# We stamp it here because it has to run every link
add_custom_command(TARGET kernel POST_BUILD
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/stamp_syms.sh
            ${KERNEL_NM} ${KERNEL_OBJCOPY} ${KERNEL_OBJDUMP}
            ${KERNEL_SYMS_RESERVE}
            $<TARGET_FILE:kernel>
    COMMENT "Stamping symbol and line tables into .kernel_syms"
    VERBATIM
)
