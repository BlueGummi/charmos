#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [[ -t 1 ]] && [[ "$(tput colors 2>/dev/null || echo 0)" -ge 8 ]]; then
    GREEN=$'\033[0;32m'
    YELLOW=$'\033[0;33m'
    RED=$'\033[0;31m'
    BLUE=$'\033[0;34m'
    BOLD=$'\033[1m'
    NC=$'\033[0m'
else
    GREEN=''; YELLOW=''; RED=''; BLUE=''; BOLD=''; NC=''
fi

QUIET=false
CLEAN=false
COMPDB=false
BUILD_TYPE="Debug"
BUILD_TYPE_EXPLICIT=false
COMPILER="gcc"
GENERATOR="auto"
BUILD_DIR="build"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
CMAKE_EXTRA_ARGS=()
TARGETS=()
FILTER=""
CMDLINE_FILE=""
NIGHTMARE=""

print_help() {
    local self="$1"
    cat <<EOF
${BOLD}charmOS build script${NC}

${BOLD}Usage:${NC} ${self} [OPTIONS] [-- CMAKE_ARGS...] [TARGETS...]

${BOLD}Options:${NC}
  -h, --help              Show this help and exit
  -q, -s, --quiet         Suppress command output
  -c, --clean             Remove build directory before building
  -C, --compdb            Generate compile_commands.json in project root
  -t, --type TYPE         Build type: Debug, Release, RelWithDebInfo, MinSizeRel
                          (default: Debug)
  -k, --compiler COMP     Compiler toolchain: gcc or clang (default: gcc)
                          clang is required for DEBUG_ASAN (KASAN)
      --clang             Shorthand for --compiler clang
  -G, --generator GEN     Build generator: ninja, make, or auto (default: auto)
      --ninja             Shorthand for --generator ninja
      --make              Shorthand for --generator make
  -j, --jobs N            Parallel jobs (default: detected CPU count = ${JOBS})
  -B, --build-dir DIR     Build directory (default: build)
  -f, --filter LIST       Restrict the tests target to LIST, space/comma separated
      --cmdline FILE      Boot with FILE's contents as the kernel command line.
      --nightmare NAME    Shorthand for a command line of "nightmare=NAME"

  --filter, --cmdline, and --nightmare are mutually exclusive

${BOLD}Targets:${NC} (passed to make)
  iso         Build bootable ISO (default if no targets given)
  run         Build and run in QEMU with graphics
  headless    Build and run in QEMU without graphics
  tests       Build and run test suite
  debug       Build and run QEMU paused for gdb
  clean-full  Wipe ISO, limine, ovmf, syms, etc.

${BOLD}Examples:${NC}
  ${self}                                  # Debug build, produce ISO
  ${self} -t Release run                   # Release build, run in QEMU
  ${self} -- -DQEMU_KVM=ON -DQEMU_NUMA=OFF run
  ${self} --clean -t RelWithDebInfo tests
  ${self} -f "apc dpc" tests               # run only the apc and dpc tests
  ${self} --cmdline n.txt iso              # boot the command line in n.txt
EOF
    exit 0
}

PASSTHROUGH=false
while [[ $# -gt 0 ]]; do
    if $PASSTHROUGH; then
        CMAKE_EXTRA_ARGS+=("$1"); shift; continue
    fi
    case "$1" in
        -h|--help|help)        print_help "$0" ;;
        -q|-s|--quiet)         QUIET=true; shift ;;
        -c|--clean)            CLEAN=true; shift ;;
        -C|--compdb)           COMPDB=true; shift ;;
        -t|--type)             BUILD_TYPE="$2"; BUILD_TYPE_EXPLICIT=true; shift 2 ;;
        -k|--compiler)         COMPILER="$2"; shift 2 ;;
        --clang)               COMPILER="clang"; shift ;;
        -G|--generator)        GENERATOR="$2"; shift 2 ;;
        --ninja)               GENERATOR="ninja"; shift ;;
        --make)                GENERATOR="make"; shift ;;
        -j|--jobs)             JOBS="$2"; shift 2 ;;
        -f|--filter)           FILTER="$2"; shift 2 ;;
        --cmdline)             CMDLINE_FILE="$2"; shift 2 ;;
        --nightmare)           NIGHTMARE="$2"; shift 2 ;;
        -B|--build-dir)        BUILD_DIR="$2"; shift 2 ;;
        --)                    PASSTHROUGH=true; shift ;;
        -*)                    echo "${RED}Unknown option: $1${NC}" >&2; exit 1 ;;
        *)                     TARGETS+=("$1"); shift ;;
    esac
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=("iso")
fi

cmdline_sources=()
[[ -n "$FILTER" ]]       && cmdline_sources+=("--filter")
[[ -n "$CMDLINE_FILE" ]] && cmdline_sources+=("--cmdline")
[[ -n "$NIGHTMARE" ]]    && cmdline_sources+=("--nightmare")
if [[ ${#cmdline_sources[@]} -gt 1 ]]; then
    echo "${RED}${cmdline_sources[*]} are mutually exclusive${NC}" >&2
    exit 1
fi

if [[ -n "$CMDLINE_FILE" ]]; then
    if [[ ! -r "$CMDLINE_FILE" ]]; then
        echo "${RED}cannot read command line file: $CMDLINE_FILE${NC}" >&2
        exit 1
    fi
    CMDLINE_FILE="$(cd "$(dirname "$CMDLINE_FILE")" && pwd)/$(basename "$CMDLINE_FILE")"
fi

case "$BUILD_TYPE" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) echo "${RED}Invalid build type: $BUILD_TYPE${NC}" >&2; exit 1 ;;
esac

case "$COMPILER" in
    gcc|clang) ;;
    *) echo "${RED}Invalid compiler: $COMPILER (expected gcc or clang)${NC}" >&2; exit 1 ;;
esac

# Resolve generator: auto prefers ninja, falls back to make.
case "$GENERATOR" in
    auto)
        if command -v ninja >/dev/null 2>&1; then GEN_KIND="ninja"; else GEN_KIND="make"; fi ;;
    ninja) GEN_KIND="ninja" ;;
    make)  GEN_KIND="make" ;;
    *) echo "${RED}Invalid generator: $GENERATOR (expected ninja, make, or auto)${NC}" >&2; exit 1 ;;
esac

if [[ "$GEN_KIND" == "ninja" ]]; then
    CMAKE_GENERATOR="Ninja"
else
    CMAKE_GENERATOR="Unix Makefiles"
fi

log()    { $QUIET || echo -e "${GREEN}==>${NC} $*"; }
warn()   { $QUIET || echo -e "${YELLOW}==>${NC} $*"; }
err()    { echo -e "${RED}error:${NC} $*" >&2; }
note()   { $QUIET || echo -e "${BLUE}    $*${NC}"; }
die()    { err "$*"; exit 1; }

run_cmd() {
    if $QUIET; then
        "$@" >/dev/null 2>&1
    else
        "$@"
    fi
}

check_tool() {
    local tool="$1"
    local hint="${2:-}"
    if ! command -v "$tool" >/dev/null 2>&1; then
        err "missing required tool: ${BOLD}${tool}${NC}"
        [[ -n "$hint" ]] && echo "       ${hint}" >&2
        return 1
    fi
}

check_required_tools() {
    log "checking required tools"
    local failed=0
    check_tool cmake    "install via your package manager (apt/dnf/brew install cmake)"   || failed=1
    check_tool make     "install build-essential / xcode-select --install"                || failed=1
    if [[ "$GEN_KIND" == "ninja" ]]; then
        check_tool ninja "install ninja (apt/dnf/brew install ninja) or use --make"        || failed=1
    fi
    check_tool nasm     "install via your package manager (apt/dnf/brew install nasm)"    || failed=1
    check_tool nm       "binutils package"                                                || failed=1
    check_tool awk      "install gawk or mawk"                                            || failed=1
    if ! command -v x86_64-elf-objcopy >/dev/null 2>&1; then
        check_tool objcopy "binutils package, or x86_64-elf-objcopy for a cross build"    || failed=1
    fi
    check_tool xorriso  "install xorriso (apt/dnf/brew install xorriso)"                  || failed=1
    check_tool git      "git is required for fetching submodules and limine"              || failed=1

    for t in "${TARGETS[@]}"; do
        case "$t" in
            run|tests|headless|debug)
                check_tool qemu-system-x86_64 "install QEMU (apt/dnf/brew install qemu)" || failed=1
                break
                ;;
        esac
    done

    if grep -RIlq --include='*.rs' "" kernel/ 2>/dev/null; then
        if ! command -v cargo >/dev/null 2>&1; then
            warn "Rust sources detected in kernel/ but cargo not found"
            warn "install via https://rustup.rs/"
            failed=1
        fi
    fi

    [[ $failed -eq 0 ]] || die "fix missing tools and rerun"
}

find_kasan_clang() {
    local c
    for c in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang clang; do
        command -v "$c" >/dev/null 2>&1 || continue
        echo "$c"
        return 0
    done
    return 1
}

check_compiler_toolchain() {
    log "verifying ${COMPILER} toolchain"
    local failed=0

    if [[ "$COMPILER" == "clang" ]]; then
        local clang_bin
        if ! clang_bin="$(find_kasan_clang)"; then
            err "clang not found"
            echo "       install LLVM: brew install llvm lld" >&2
            failed=1
        else
            note "using clang: ${clang_bin}"
            if ! echo 'int x;' | "$clang_bin" --target=x86_64-unknown-linux-elf \
                    -fsanitize=kernel-address -ffreestanding -mno-red-zone \
                    -c -x c - -o /dev/null >/dev/null 2>&1; then
                err "${clang_bin} cannot compile with -fsanitize=kernel-address"
                echo "       Apple Clang lacks KASAN; install Homebrew LLVM: brew install llvm" >&2
                failed=1
            fi
        fi

        if ! command -v ld.lld >/dev/null 2>&1 && \
           ! command -v x86_64-elf-ld >/dev/null 2>&1; then
            err "no ELF linker found for clang builds (need ld.lld or x86_64-elf-ld)"
            echo "       brew install lld" >&2
            failed=1
        fi
    else
        if [[ "$(uname -s)" == "Darwin" ]]; then
            check_tool x86_64-elf-gcc "install the cross GCC: brew install x86_64-elf-gcc" || failed=1
        elif [[ "$(uname -m)" == "aarch64" ]]; then
            check_tool x86_64-linux-gnu-gcc "install the cross GCC toolchain (gcc-x86-64-linux-gnu)" || failed=1
        else
            check_tool gcc "install gcc via your package manager" || failed=1
        fi
    fi

    [[ $failed -eq 0 ]] || die "fix the ${COMPILER} toolchain and rerun"
}

select_toolchain() {
    TOOLCHAIN_ARGS=()
    if [[ "$COMPILER" == "clang" ]]; then
        note "using clang toolchain (cmake/toolchains/clang.cmake)"
        TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchains/clang.cmake")
    elif [[ "$(uname -s)" == "Darwin" ]]; then
        note "detected macOS — using cmake/toolchains/cross-elf.cmake"
        TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchains/cross-elf.cmake")
    elif [[ "$(uname -m)" == "aarch64" ]]; then
        note "detected aarch64 host — using cross toolchain"
        TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchains/cross-linux-gnu.cmake")
    fi
}

configure_cmake() {
    local type_args=()
    if $BUILD_TYPE_EXPLICIT || [[ ! -f "CMakeCache.txt" ]]; then
        type_args=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")
    fi

    run_cmd cmake \
        -G "$CMAKE_GENERATOR" \
        "${type_args[@]}" \
        "${TOOLCHAIN_ARGS[@]}" \
        "${CMAKE_EXTRA_ARGS[@]}" \
        "$REPO_ROOT"
}

cached_build_type() {
    [[ -f "CMakeCache.txt" ]] &&
        sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "CMakeCache.txt"
    return 0
}

build_targets() {
    run_cmd cmake --build . -j"$JOBS" --target "$@"
}

check_required_tools
check_compiler_toolchain

log "syncing submodules"
run_cmd git submodule update --init --recursive
rm -rf kernel/uACPI/tests 2>/dev/null || true

if [[ ! -d "limine" ]]; then
    warn "fetching limine bootloader"
    run_cmd git clone https://github.com/limine-bootloader/limine \
        --branch=v9.x-binary --depth=1
fi

if $CLEAN && [[ -d "$BUILD_DIR" ]]; then
    warn "removing existing $BUILD_DIR/"
    rm -rf "$BUILD_DIR"
fi

# CMake refuses to switch generators in an existing build dir; wipe on mismatch
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_gen="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [[ -n "$cached_gen" && "$cached_gen" != "$CMAKE_GENERATOR" ]]; then
        warn "generator changed ($cached_gen -> $CMAKE_GENERATOR); wiping $BUILD_DIR/"
        rm -rf "$BUILD_DIR"
    fi
fi

stale_toolchain=""
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_tc="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [[ -n "$cached_tc" && ! -f "$cached_tc" ]]; then
        stale_toolchain="$cached_tc"
    fi
fi
if [[ -z "$stale_toolchain" ]]; then
    shopt -s nullglob
    for sysfile in "$BUILD_DIR"/CMakeFiles/*/CMakeSystem.cmake; do
        while IFS= read -r included; do
            [[ -n "$included" && ! -f "$included" ]] || continue
            stale_toolchain="$included"
            break
        done < <(sed -n 's/^include("\(.*\)")$/\1/p' "$sysfile")
        [[ -n "$stale_toolchain" ]] && break
    done
    shopt -u nullglob
fi
if [[ -n "$stale_toolchain" ]]; then
    warn "toolchain file is gone ($stale_toolchain); wiping $BUILD_DIR/"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

select_toolchain

effective_type="$(cached_build_type)"
if $BUILD_TYPE_EXPLICIT || [[ -z "$effective_type" ]]; then
    effective_type="$BUILD_TYPE"
else
    note "keeping cached build type ${effective_type} (no -t given)"
fi

log "configuring cmake (${effective_type}, ${CMAKE_GENERATOR})"
configure_cmake

if [[ "$GEN_KIND" == "ninja" ]]; then
    flag_files=(build.ninja)
else
    shopt -s nullglob globstar
    flag_files=(CMakeFiles/**/flags.make)
    shopt -u nullglob globstar
fi

# grep exits 1 on no match and 2 on a missing file
opt_level=""
if [[ ${#flag_files[@]} -gt 0 ]]; then
    opt_level="$(grep -h -m1 -oE '(^| )-O[0-3sgz]' "${flag_files[@]}" 2>/dev/null | head -n1 | tr -d ' ' || true)"
fi
log "build type ${effective_type}${opt_level:+, compiling at ${opt_level}}"

if $COMPDB; then
    log "generating compile_commands.json"
    run_cmd cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$REPO_ROOT"

    compdb_link="$REPO_ROOT/compile_commands.json"
    compdb_target="$PWD/compile_commands.json"
    case "$PWD" in
        "$REPO_ROOT"/*) compdb_target="${PWD#"$REPO_ROOT"/}/compile_commands.json" ;;
    esac

    if [[ ! -L "$compdb_link" ]] ||
       [[ "$(readlink "$compdb_link")" != "$compdb_target" ]]; then
        ln -sfn "$compdb_target" "$compdb_link"
    fi
fi


if [[ -n "$CMDLINE_FILE" ]]; then
    # cmake reads it, hand over a path it can resolve from any directory
    CMDLINE_FILE="$(cd "$(dirname "$CMDLINE_FILE")" && pwd)/$(basename "$CMDLINE_FILE")"
    export CMDLINE="$CMDLINE_FILE"
    note "cmdline file: ${CMDLINE_FILE}"
elif [[ -n "$NIGHTMARE" ]]; then
    export NIGHTMARE_TESTS="$NIGHTMARE"
    note "nightmare test: ${NIGHTMARE}"
elif [[ -n "$FILTER" ]]; then
    export TESTS="$FILTER"
    note "test filter: ${FILTER}"
    case " ${TARGETS[*]} " in
        *" tests "*) ;;
        *) warn "--filter set but 'tests' is not among the targets; it will have no effect" ;;
    esac
fi

log "building targets: ${TARGETS[*]}"
build_targets "${TARGETS[@]}"

log "${BOLD}done${NC}"
