FROM ubuntu:24.04

RUN apt update -y && apt upgrade -y && \
    apt install -y xorriso nasm qemu-system gdb make gcc clang lld llvm git cmake binutils \
                   python3 python3-jsonschema nodejs && \
    rm -rf /var/lib/apt/lists/*

RUN git config --system --add safe.directory '*'

WORKDIR /workspace
