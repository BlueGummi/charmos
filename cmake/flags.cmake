string(TOUPPER "${CMAKE_BUILD_TYPE}" _BUILD_TYPE_UPPER)
if(_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    add_compile_definitions(BUILD_DEBUG)
elseif(_BUILD_TYPE_UPPER STREQUAL "RELEASE" OR _BUILD_TYPE_UPPER STREQUAL "MINSIZEREL")
    add_compile_definitions(BUILD_RELEASE)
elseif(_BUILD_TYPE_UPPER STREQUAL "RELWITHDEBINFO")
    add_compile_definitions(BUILD_RELEASE)
endif()

set(KERNEL_WARNINGS
    -Wall -Wextra
    -Wpointer-sign -Wenum-compare
)

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    list(APPEND KERNEL_WARNINGS -Wno-initializer-overrides)
else()
    list(APPEND KERNEL_WARNINGS -Wno-override-init)
endif()

set(KERNEL_WARNINGS_RELEASE
    -Wunused
    -Wno-unused-parameter
    -Wvla
    -Wnull-dereference
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND KERNEL_WARNINGS_RELEASE
        -Wshadow=local
        -Wlogical-op
        -Wduplicated-cond
        -Wduplicated-branches
    )
endif()

string(REPLACE ";" " " KERNEL_WARNINGS_RELEASE_STR "${KERNEL_WARNINGS_RELEASE}")

set(KERNEL_WARNINGS_DEBUG
    -Wno-unused-parameter
    -Wno-unused-function
)

string(REPLACE ";" " " KERNEL_WARNINGS_DEBUG_STR "${KERNEL_WARNINGS_DEBUG}")

set(KERNEL_FREESTANDING
    -ffreestanding
    -fno-stack-protector -fno-stack-check
    -fno-PIC -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -ffunction-sections -fdata-sections
)

set(KERNEL_TARGET_ARCH
    -m64 -mcmodel=kernel
    -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow
    -mno-red-zone
    -mgeneral-regs-only
)

set(KERNEL_DEFINES
    -DLIMINE_API_REVISION=2
    -DUACPI_DEFAULT_LOG_LEVEL=4
)

set(KERNEL_RELFILE_OK FALSE)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 8)
    set(KERNEL_RELFILE_OK TRUE)
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 10)
    set(KERNEL_RELFILE_OK TRUE)
endif()

if(KERNEL_RELFILE_OK)
    list(APPEND KERNEL_DEFINES -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=)
else()
    message(WARNING "charmOS: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} "
                    "lacks -fmacro-prefix-map; __RELFILE__ will hold absolute paths")
endif()

list(APPEND KERNEL_DEFINES -D__RELFILE__=__FILE__)

set(CMAKE_C_FLAGS_DEBUG          "-O0 -ggdb -g3 ${KERNEL_WARNINGS_DEBUG_STR}")
set(CMAKE_C_FLAGS_RELEASE        "-O3 ${KERNEL_WARNINGS_RELEASE_STR}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -ggdb -g3")
set(CMAKE_C_FLAGS_MINSIZEREL     "-Os")

option(KERNEL_STACK_USAGE "Emit .su files with -fstack-usage" ON)
if(KERNEL_STACK_USAGE)
    list(APPEND KERNEL_FREESTANDING -fstack-usage)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND KERNEL_FREESTANDING -malign-data=abi)
endif()

set(KERNEL_C_FLAGS_LIST
    ${KERNEL_WARNINGS}
    ${KERNEL_FREESTANDING}
    ${KERNEL_TARGET_ARCH}
    ${KERNEL_DEFINES}
    -pipe
)

string(REPLACE ";" " " KERNEL_C_FLAGS_STR "${KERNEL_C_FLAGS_LIST}")
set(CMAKE_C_FLAGS "${KERNEL_C_FLAGS_STR} -std=gnu11")

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_ASM_NASM_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if(APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -target x86_64-unknown-unknown")
    set(CMAKE_LINKER "x86_64-elf-ld")
    set(CMAKE_EXE_LINKER_FLAGS
        "-nostdlib -static -Wl,--gc-sections -z max-page-size=0x1000 -T ${CMAKE_SOURCE_DIR}/kernel/linker-${ARCH}.ld")
else()
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -Wl,--build-id=none -nostdlib -static -z max-page-size=0x1000 -Wl,--gc-sections -T ${CMAKE_SOURCE_DIR}/kernel/linker-${ARCH}.ld")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    find_program(LLD_LINKER ld.lld)
    if(LLD_LINKER)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
    else()
        find_program(ELF_LD x86_64-elf-ld)
        if(NOT ELF_LD)
            message(FATAL_ERROR
                "clang build needs an ELF linker: install lld (brew install lld) "
                "or x86_64-elf-ld")
        endif()
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=${ELF_LD}")
    endif()
endif()

option(DEBUG_ASAN "Enable KASAN address sanitizer (clang only)" OFF)
if(DEBUG_ASAN)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "DEBUG_ASAN (KASAN) requires the clang toolchain. Reconfigure with:\n"
            "    scripts/build.sh --compiler clang -- -DDEBUG_ASAN=ON")
    endif()
    message(STATUS "charmOS: KASAN enabled (DEBUG_ASAN, outline instrumentation)")
    add_compile_definitions(DEBUG_ASAN)
    set(KERNEL_KASAN_FLAGS
        -fsanitize=kernel-address
        -mllvm -asan-instrumentation-with-call-threshold=0
        -mllvm -asan-globals=0
        -mllvm -asan-stack=0
    )
    string(REPLACE ";" " " KERNEL_KASAN_FLAGS_STR "${KERNEL_KASAN_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${KERNEL_KASAN_FLAGS_STR}")
endif()

option(KERNEL_RANDCONFIG "Append a random subset of codegen flags (build fuzz)" OFF)
set(KERNEL_RANDCONFIG_SEED "0" CACHE STRING "Seed for KERNEL_RANDCONFIG selection")
if(KERNEL_RANDCONFIG)
    set(KERNEL_RANDCONFIG_POOL
        -funroll-loops
        -fno-strict-aliasing
        -fwrapv
        -fno-jump-tables
        -fno-common
        -fmerge-all-constants
        -fno-asynchronous-unwind-tables
        -fno-strict-overflow
        -finline-functions
        -fno-inline-functions
    )

    string(RANDOM LENGTH 1 ALPHABET "01" RANDOM_SEED "${KERNEL_RANDCONFIG_SEED}" _rc_ignore)
    set(KERNEL_RANDCONFIG_CHOSEN "")
    foreach(_rc_flag ${KERNEL_RANDCONFIG_POOL})
        string(RANDOM LENGTH 1 ALPHABET "01" _rc_bit)
        if(_rc_bit STREQUAL "1")
            list(APPEND KERNEL_RANDCONFIG_CHOSEN "${_rc_flag}")
        endif()
    endforeach()
    if(KERNEL_RANDCONFIG_CHOSEN)
        string(REPLACE ";" " " _rc_str "${KERNEL_RANDCONFIG_CHOSEN}")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_rc_str}")
    endif()
    message(STATUS "charmOS: RANDCONFIG seed=${KERNEL_RANDCONFIG_SEED} "
                   "flags=[${KERNEL_RANDCONFIG_CHOSEN}]")
endif()

enable_language(ASM_NASM)
set(CMAKE_ASM_NASM_FLAGS "-F dwarf -g -Wall -f elf64 -Wno-reloc-rel-dword")
