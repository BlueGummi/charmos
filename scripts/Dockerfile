FROM ubuntu:24.04

ARG LLVM_VERSION=22
ARG GCC_VERSION=14

RUN apt update -y && apt upgrade -y && \
    apt install -y --no-install-recommends ca-certificates curl gnupg && \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
         -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc && \
    echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
         > /etc/apt/sources.list.d/llvm.list && \
    apt update -y && \
    apt install -y xorriso nasm qemu-system gdb make gcc gcc-${GCC_VERSION} git cmake binutils \
                   python3 nodejs \
                   clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} && \
    update-alternatives \
        --install /usr/bin/clang clang /usr/bin/clang-${LLVM_VERSION} 100 \
        --slave /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} \
        --slave /usr/bin/lld lld /usr/bin/lld-${LLVM_VERSION} \
        --slave /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-${LLVM_VERSION} \
        --slave /usr/bin/llvm-symbolizer llvm-symbolizer \
                /usr/bin/llvm-symbolizer-${LLVM_VERSION} \
        --slave /usr/bin/llvm-addr2line llvm-addr2line \
                /usr/bin/llvm-addr2line-${LLVM_VERSION} \
        --slave /usr/bin/llvm-nm llvm-nm /usr/bin/llvm-nm-${LLVM_VERSION} \
        --slave /usr/bin/llvm-objdump llvm-objdump \
                /usr/bin/llvm-objdump-${LLVM_VERSION} && \
    for t in gcc cpp gcc-ar gcc-nm gcc-ranlib gcov; do \
        ln -sf "${t}-${GCC_VERSION}" "/usr/bin/${t}"; \
    done && \
    rm -rf /var/lib/apt/lists/*

RUN git config --system --add safe.directory '*'

WORKDIR /workspace
