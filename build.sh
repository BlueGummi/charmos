#!/bin/bash

GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

if [ -f .gitmodules ]; then
    if [ ! -d ".git/modules" ]; then
        echo -e "${YELLOW}Initializing Git submodules...${NC}"
        git submodule init && git submodule update
    else
        echo -e "${GREEN}Git submodules are already initialized.${NC}"
    fi
else
    echo -e "${RED}No .gitmodules file found, skipping submodule initialization.${NC}"
fi

if [ ! -d "limine" ]; then
    echo -e "${YELLOW}Cloning Limine...${NC}"
    git clone https://github.com/limine-bootloader/limine --branch=v9.x-binary --depth=1
else
    echo -e "${GREEN}Limine is already cloned.${NC}"
fi

if [ -f "./kernel/get-deps" ]; then
    echo -e "${YELLOW}Fetching dependencies...${NC}"
    ./kernel/get-deps
else
    echo -e "${RED}Dependency script not found! Skipping...${NC}"
fi

if [ ! -d "build" ]; then
    echo -e "${YELLOW}Creating build directory...${NC}"
    mkdir -p build
else
    echo -e "${GREEN}Build directory already exists.${NC}"
fi

cd build || exit

# Run CMake
if [ ! -f "CMakeCache.txt" ]; then
    echo -e "${YELLOW}Running CMake...${NC}"
    cmake ..
else
    echo -e "${GREEN}CMake is already configured.${NC}"
fi

echo -e "${YELLOW}Building...${NC}"
make iso

echo -e "${GREEN}Build complete!${NC}"

