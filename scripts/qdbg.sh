#!/usr/bin/env bash
# Attach gdb to running QEMU guest and dump backtraces
#
# QEMU gets started with `-qmp unix:/tmp/qmp.sock`, so
# we just hook into that here
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SOCK="${QMP_SOCK:-/tmp/qmp.sock}"
PORT="${GDB_PORT:-1234}"
KERNEL="${KERNEL_ELF:-}"
GDB="${GDB:-gdb}"

usage() {
    cat <<EOF
usage: $(basename "$0") [-B BUILD_DIR] [-k KERNEL_ELF] [-s QMP_SOCK] [-p PORT] [-- GDB_CMDS...]

Dumps 'thread apply all bt' for every vCPU. Extra gdb commands may be passed
after --, e.g.:

  $(basename "$0") -B build-gcc -- 'info registers' 'p \$_siginfo'

env: QMP_SOCK, GDB_PORT, KERNEL_ELF, GDB
EOF
    exit "${1:-0}"
}

BUILD_DIR=""
EXTRA=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -B|--build-dir) BUILD_DIR="$2"; shift 2 ;;
        -k|--kernel)    KERNEL="$2";    shift 2 ;;
        -s|--sock)      SOCK="$2";      shift 2 ;;
        -p|--port)      PORT="$2";      shift 2 ;;
        -h|--help)      usage 0 ;;
        --)             shift; EXTRA=("$@"); break ;;
        *)              echo "unknown argument: $1" >&2; usage 1 ;;
    esac
done

# Verify the ELF matches
if [[ -z "$KERNEL" ]]; then
    if [[ -n "$BUILD_DIR" ]]; then
        KERNEL="$BUILD_DIR/kernel/kernel"
    else
        mapfile -t found < <(find "$REPO_ROOT" -maxdepth 3 -type f -path '*/kernel/kernel' 2>/dev/null)
        if [[ ${#found[@]} -eq 1 ]]; then
            KERNEL="${found[0]}"
        elif [[ ${#found[@]} -gt 1 ]]; then
            echo "qdbg: several kernel ELFs found, pass -B or -k:" >&2
            printf '  %s\n' "${found[@]}" >&2
            exit 2
        fi
    fi
fi

[[ -n "$KERNEL" && -f "$KERNEL" ]] || { echo "qdbg: no kernel ELF (pass -B or -k)" >&2; exit 2; }
[[ -S "$SOCK" ]] || { echo "qdbg: no QMP socket at $SOCK -- is the guest running?" >&2; exit 2; }

echo "qdbg: kernel  $KERNEL" >&2
echo "qdbg: elf mtime $(date -r "$KERNEL" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || stat -c %y "$KERNEL")" >&2
echo "qdbg: qmp     $SOCK -> gdbserver tcp::$PORT" >&2

# Greet, negotiate caps, HMP passthrough
python3 - "$SOCK" "$PORT" <<'PY'
import json, socket, sys

sock_path, port = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sock_path)
f = s.makefile("rw", encoding="utf-8", newline="\n")

def rpc(obj):
    f.write(json.dumps(obj) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise SystemExit("qdbg: QMP closed unexpectedly")
        msg = json.loads(line)
        if "event" in msg:          # async events get skipped 
            continue
        return msg

greeting = json.loads(f.readline())
ver = greeting.get("QMP", {}).get("version", {}).get("qemu", {})
print("qdbg: qemu {}.{}.{}".format(ver.get("major"), ver.get("minor"), ver.get("micro")),
      file=sys.stderr)

rpc({"execute": "qmp_capabilities"})
r = rpc({"execute": "human-monitor-command",
         "arguments": {"command-line": "gdbserver tcp::%s" % port}})
if "error" in r:
    raise SystemExit("qdbg: %s" % r["error"].get("desc", r["error"]))
out = (r.get("return") or "").strip()
if out:
    print("qdbg: %s" % out, file=sys.stderr)
PY

CMDS=(-ex "set pagination off"
      -ex "set confirm off"
      -ex "target remote :$PORT"
      -ex "info threads"
      -ex "thread apply all bt")
for c in "${EXTRA[@]:-}"; do
    [[ -n "$c" ]] && CMDS+=(-ex "$c")
done
CMDS+=(-ex "detach" -ex "quit")

"$GDB" -q -batch "${CMDS[@]}" "$KERNEL"
