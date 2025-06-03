cmake_minimum_required(VERSION 3.16)
project(charmOS-kernel C CXX ASM_NASM)
set(CARGO_TARGET x86_64-unknown-none)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_language(ASM_NASM)

file(GLOB_RECURSE CPP_SOURCES *.cpp *.cc)
file(GLOB_RECURSE C_SOURCES *.c)
file(GLOB_RECURSE ASM_SOURCES *.S)
file(GLOB_RECURSE NASM_SOURCES *.asm)
file(GLOB_RECURSE RS_SOURCES *.rs)

include_directories(. freestnd-c-hdrs)
include(uACPI/uacpi.cmake)
add_executable(kernel ${C_SOURCES} ${ASM_SOURCES} ${NASM_SOURCES} ${CPP_SOURCES} ${RS_SOURCES})

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/kernel_rust.a
    COMMAND cargo build --release --target ${CARGO_TARGET} --quiet
    COMMAND cp ${CMAKE_SOURCE_DIR}/kernel/rust/target/${CARGO_TARGET}/release/libkernel_rust.a ${CMAKE_BINARY_DIR}/kernel_rust.a
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/kernel/rust
    DEPENDS ${RS_SOURCES}
    COMMENT "Compiling rust things"
    VERBATIM
)

add_custom_target(kernel_rust_build DEPENDS ${CMAKE_BINARY_DIR}/kernel_rust.a)

add_library(kernel_rust STATIC IMPORTED)
set_target_properties(kernel_rust PROPERTIES
    IMPORTED_LOCATION ${CMAKE_BINARY_DIR}/kernel_rust.a
)

add_dependencies(kernel kernel_rust_build)
target_link_libraries(kernel kernel_rust)

target_sources(
    kernel
    PRIVATE
    ${UACPI_SOURCES}
)

target_include_directories(
    kernel
    PRIVATE
    ${UACPI_INCLUDES}
)
