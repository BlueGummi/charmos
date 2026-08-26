FROM ubuntu:24.04

RUN apt update -y && apt upgrade -y && \
    apt install -y xorriso nasm qemu-system gdb make gcc git cmake binutils \
                   python3 python3-jsonschema nodejs llvm && \
    rm -rf /var/lib/apt/lists/*

RUN git config --system --add safe.directory '*'

WORKDIR /workspace
