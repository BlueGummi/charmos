FROM ubuntu:24.04
RUN apt update -y && apt upgrade -y && \
    apt install -y xorriso nasm qemu-system gdb make gcc git cmake binutils && \
    rm -rf /var/lib/apt/lists/*

RUN git config --system --add safe.directory '*'

WORKDIR /workspace
