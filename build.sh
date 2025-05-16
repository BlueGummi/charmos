#!/bin/bash
cli_quiet_arg=''
make_quiet_arg=''
cmake_quiet_arg=''

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

if [ ! -d "limine" ]; then
    cond_print "${YELLOW}downloading limine${NC}"
    git clone https://github.com/limine-bootloader/limine --branch=v9.x-binary --depth=1 $cli_quiet_arg
fi


if [ ! -d "kernel/uACPI" ]; then
    cond_print "${YELLOW}downloading uacpi${NC}"
    cd kernel
    git clone https://github.com/uacpi/uACPI --depth=1 $cli_quiet_arg
    cd -
fi

if [ ! -d "kernel/flanterm" ]; then
    cond_print "${YELLOW}downloading flanterm${NC}"
    cd kernel
    git clone https://codeberg.org/mintsuki/flanterm --depth=1 $cli_quiet_arg
    cd -
fi

./kernel/get-deps

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

cond_print "${YELLOW}Building...${NC}"
if [ "$make_quiet_arg" ]; then
    make iso 2>&1 >/dev/null
else
    make iso
fi
