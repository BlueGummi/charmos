cmake_minimum_required(VERSION 3.16)
project(charmOS-kernel C ASM_NASM)
set(CARGO_TARGET x86_64-unknown-none)
set(CMAKE_C_STANDARD 11)

enable_language(ASM_NASM)

file(GLOB_RECURSE C_SOURCES src/*.c)
file(GLOB_RECURSE ASM_SOURCES src/*.S)
file(GLOB_RECURSE NASM_SOURCES src/*.asm)
file(GLOB_RECURSE RS_SOURCES src/rust/src/*.rs)

include_directories(src freestnd-c-hdrs)
include(src/uACPI/uacpi.cmake)
add_executable(kernel ${C_SOURCES} ${ASM_SOURCES} ${NASM_SOURCES})

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/kernel_rust.a
    COMMAND cargo build --release --target ${CARGO_TARGET} --quiet
    COMMAND cp ${CMAKE_SOURCE_DIR}/kernel/src/rust/target/${CARGO_TARGET}/release/libkernel_rust.a ${CMAKE_BINARY_DIR}/kernel_rust.a
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/kernel/src/rust
    COMMENT "Building Rust kernel module"
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
