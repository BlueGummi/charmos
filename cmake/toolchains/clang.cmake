set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(CHARMOS_CLANG
    NAMES clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    NO_DEFAULT_PATH)
if(NOT CHARMOS_CLANG)
    find_program(CHARMOS_CLANG NAMES clang)
endif()
if(NOT CHARMOS_CLANG)
    message(FATAL_ERROR
        "clang not found. Install LLVM (e.g. `brew install llvm lld`).")
endif()

set(CMAKE_C_COMPILER   "${CHARMOS_CLANG}")
set(CMAKE_CXX_COMPILER "${CHARMOS_CLANG}++")

set(CMAKE_C_COMPILER_TARGET   x86_64-unknown-linux-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-linux-elf)
set(CMAKE_ASM_COMPILER_TARGET x86_64-unknown-linux-elf)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
