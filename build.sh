#!/bin/bash
cli_quiet_arg=''
make_quiet_arg=''
cmake_quiet_arg=''

# TODO: Change this to account for rust things and whether or not the compiler exists

print_help() {
    local color_supported=$(tput colors 2>/dev/null)

    if [[ -t 1 && (${color_supported:-0} -ge 8) ]]; then
        underline=$(tput smul)
        reset=$(tput sgr0)
    else
        underline=""
        reset=""
    fi

    printf "The build script for charmOS\n\n"
    printf "${underline}Usage${reset}: $1 [OPTIONS] [TARGETS]\n"
    printf "Options:\n"
    printf "  -c, --cleanup      Clean directories after building\n"
    printf "  -q, -s, --quiet    Suppress command output\n"
    exit 0
}

for arg in "$@"; do
    case $arg in
        --quiet|-q|-s)
            quiet=true
            cli_quiet_arg='--quiet'
            make_quiet_arg=true
            cmake_quiet_arg=true
            ;;
        --help|-h|help)
            print_help "$0"
            ;;
        --clean|-c)
            clean=true
            ;;
    esac
done

GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

cond_print() {
    if [ ! "$quiet" ]; then
        echo -e "$1"
    fi
}

cond_print "${GREEN}getting dependencies${NC}"

git submodule init && git submodule update
rm -rf kernel/uACPI/tests
if [ ! -d "limine" ]; then
    cond_print "${YELLOW}downloading limine${NC}"
    git clone https://github.com/limine-bootloader/limine --branch=v9.x-binary --depth=1 $cli_quiet_arg
fi

if [ ! -d "build" ]; then
    mkdir -p build
fi

cd build || exit

cond_print "${GREEN}configuring cmake${NC}"

if [ ! -f "CMakeCache.txt" ]; then
    if uname -s | grep -q "Darwin"; then
        if [ "$cmake_quiet_arg" ]; then 
            cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake .. 2>&1 >/dev/null
        else
            cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake ..
        fi
    fi
    if [ "$cmake_quiet_arg" ]; then 
        cmake .. 2>&1 >/dev/null
    else
        cmake ..
    fi
fi

cond_print "${YELLOW}First initial build...${NC}"
if [ "$make_quiet_arg" ]; then
    make 2>&1 >/dev/null
else
    make 
fi

nm "kernel/kernel" | awk -f "../script.awk" > "../syms/fullsyms.c"

cond_print "${YELLOW}Build after symbol table creation...${NC}"
if [ "$make_quiet_arg" ]; then
    cmake ..
    make iso 2>&1 >/dev/null
else
    cmake ..
    make iso
fi


