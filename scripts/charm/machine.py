"""The machine charmOS boots on

definition (scripts/machines/*.toml)
"""

import argparse
import re
import shutil
import subprocess
import sys
import tomllib
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .paths import machines_dir

DEBUG_EXIT_DEVICE = "isa-debug-exit,iobase=0xf4,iosize=0x04"

_SUFFIXES = {"K": 1 / 1024, "M": 1, "G": 1024, "T": 1024 * 1024}


class MachineError(ValueError):
    pass


def parse_memory(text: str | int) -> int:
    """Memory as MiB"""
    if isinstance(text, int):
        return text
    match = re.fullmatch(r"\s*(\d+)\s*([KMGT])?B?\s*", str(text), re.IGNORECASE)
    if not match:
        raise MachineError(f"{text!r} is not a memory size (try 8G or 2048M)")
    amount, suffix = int(match.group(1)), (match.group(2) or "M").upper()
    return int(amount * _SUFFIXES[suffix])


@dataclass(frozen=True)
class Smp:
    # read by pydantic when a suite embeds one; a plain dict keeps this module
    # importable without it
    __pydantic_config__ = {"extra": "forbid"}  # noqa: RUF012

    sockets: int = 1
    cores: int = 1
    threads: int = 1

    @property
    def total(self) -> int:
        return self.sockets * self.cores * self.threads

    def topo(self) -> str:
        return f"sockets={self.sockets},cores={self.cores},threads={self.threads}"


@dataclass(frozen=True)
class Machine:
    name: str
    arch: str
    type: str
    min_qemu: str
    nodefaults: bool
    memory_mib: int
    smp: Smp
    numa_nodes: int
    numa_distances: tuple[tuple[int, int, int], ...]
    devices: dict[str, Any]
    acpi: dict[str, Any]
    trace: dict[str, Any]
    modes: dict[str, Any] = field(default_factory=dict)

    @property
    def qemu(self) -> str:
        return f"qemu-system-{self.arch}"


def load(profile: str = "default", *, directory: Path | None = None) -> Machine:
    path = (directory or machines_dir()) / f"{profile}.toml"
    try:
        document = tomllib.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise MachineError(f"no machine profile at {path}") from error
    except tomllib.TOMLDecodeError as error:
        raise MachineError(f"{path}: {error}") from error

    machine = document.get("machine", {})
    numa = document.get("numa", {})
    smp = Smp(**machine.get("smp", {}))

    for required in ("type", "min_qemu"):
        if required not in machine:
            raise MachineError(f"{path}: [machine] is missing {required}")

    return Machine(
        name=profile,
        arch=document.get("arch", "x86_64"),
        type=machine["type"],
        min_qemu=str(machine["min_qemu"]),
        nodefaults=bool(machine.get("nodefaults", False)),
        memory_mib=parse_memory(machine.get("memory", "2G")),
        smp=smp,
        numa_nodes=int(numa.get("nodes", 0)),
        numa_distances=tuple(tuple(d) for d in numa.get("distances", ())),
        devices=document.get("devices", {}),
        acpi=document.get("acpi", {}),
        trace=document.get("trace", {}),
        modes=document.get("modes", {}),
    )


def installed_qemu_version(machine: Machine) -> str:
    binary = shutil.which(machine.qemu)
    if binary is None:
        raise MachineError(f"{machine.qemu} is not on PATH")
    output = subprocess.run(
        [binary, "-version"], capture_output=True, text=True, check=False
    ).stdout
    found = re.search(r"version (\d+\.\d+(?:\.\d+)?)", output)
    if not found:
        raise MachineError(f"cannot read a version out of `{machine.qemu} -version`")
    return found.group(1)


def resolve_machine_type(machine: Machine) -> str:
    binary = shutil.which(machine.qemu)
    if binary is None:
        return machine.type
    output = subprocess.run(
        [binary, "-M", "help"], capture_output=True, text=True, check=False
    ).stdout
    found = re.search(
        rf"^{re.escape(machine.type)}\s+.*\(alias of (\S+)\)", output, re.MULTILINE
    )
    return found.group(1) if found else machine.type


def _version_tuple(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in text.split("."))


def check_qemu_version(machine: Machine) -> str:
    """Refuse a QEMU too old for the pinned machine type."""
    installed = installed_qemu_version(machine)
    if _version_tuple(installed) < _version_tuple(machine.min_qemu):
        raise MachineError(
            f"{machine.qemu} is {installed}, but profile {machine.name!r} pins "
            f"machine type {machine.type} and needs {machine.min_qemu} or newer"
        )
    return installed


def _numa(machine: Machine, memory_mib: int, smp: Smp) -> list[str]:
    """Node sizes and CPU ranges are derived"""
    nodes = machine.numa_nodes
    if not nodes:
        return []
    if smp.total < nodes:
        return []

    per_node_mib, remainder = divmod(memory_mib, nodes)
    if remainder:
        raise MachineError(
            f"{memory_mib}M does not divide evenly across {nodes} NUMA nodes"
        )
    cpus_per_node, remainder = divmod(smp.total, nodes)
    if remainder:
        raise MachineError(
            f"{smp.total} CPUs do not divide evenly across {nodes} NUMA nodes"
        )

    argv: list[str] = []
    for index in range(nodes):
        argv += ["-object", f"memory-backend-ram,size={per_node_mib}M,id=mem{index}"]
    for index in range(nodes):
        first = index * cpus_per_node
        argv += [
            "-numa",
            f"node,cpus={first}-{first + cpus_per_node - 1},"
            f"nodeid={index},memdev=mem{index}",
        ]
    for src, dst, value in machine.numa_distances:
        for a, b in ((src, dst), (dst, src)):
            argv += ["-numa", f"dist,src={a},dst={b},val={value}"]
    return argv


def _devices(machine: Machine, disk: Path | str | None) -> list[str]:
    argv: list[str] = []
    devices = machine.devices

    vga = devices.get("vga", {})
    if vga.get("enabled") and machine.nodefaults:
        # q35 would attach this implicitly; -nodefaults removed it
        argv += ["-device", vga.get("device", "VGA")]

    usb = devices.get("usb", {})
    if usb.get("enabled"):
        controller = usb.get("device", "qemu-xhci")
        argv += ["-device", f"{controller},id=xhci"]
        if usb.get("keyboard"):
            argv += ["-device", "usb-kbd,bus=xhci.0,port=1,id=usbkbd"]
        if usb.get("mouse"):
            argv += ["-device", "usb-mouse,bus=xhci.0,port=2,id=usbmouse"]

    iommu = devices.get("iommu", {})
    if iommu.get("enabled"):
        suffix = ",intremap=on" if iommu.get("intremap") else ""
        argv += ["-device", f"{iommu.get('device', 'intel-iommu')}{suffix}"]

    if not devices.get("nic", {}).get("enabled", False) and not machine.nodefaults:
        argv += ["-nic", "none"]

    nvme = devices.get("nvme", {})
    if nvme.get("enabled") and disk is not None:
        argv += [
            "-drive",
            f"id=nvme0,file={disk},format=raw,if=none",
            "-device",
            f"nvme,serial={nvme.get('serial', 'boom')},drive=nvme0",
        ]
    return argv


def _mode_wiring(
    mode: dict[str, Any], machine_log: Path | str | None, gdb_override: str | None
) -> list[str]:
    argv: list[str] = []

    # display is about the HOST window only
    display = mode.get("display", "none")
    if display == "nographic":
        argv += ["-nographic"]
    elif display == "none":
        argv += ["-display", "none"]
    elif display != "window":
        raise MachineError(f"unknown display mode {display!r}")

    gdb = gdb_override or mode.get("gdb")
    if gdb == "wait":
        argv += ["-s", "-S"]
    elif gdb == "listen":
        argv += ["-s"]
    elif gdb is not None:
        raise MachineError(f"unknown gdb mode {gdb!r}")

    argv += ["-serial", mode.get("console", "stdio")]
    if mode.get("no_shutdown"):
        argv += ["-no-shutdown"]
    if mode.get("no_reboot"):
        argv += ["-no-reboot"]
    if mode.get("debug_exit"):
        argv += ["-device", DEBUG_EXIT_DEVICE]
    if mode.get("machine_channel"):
        if machine_log is None:
            raise MachineError("this mode wants a machine channel but no log path")
        argv += ["-serial", f"file:{machine_log}"]
    return argv


def render(
    machine: Machine,
    mode_name: str,
    *,
    iso: Path | str,
    disk: Path | str | None = None,
    qmp_socket: Path | str,
    machine_log: Path | str | None = None,
    trace_log: Path | str | None = None,
    acpi_dir: Path | None = None,
    memory_mib: int | None = None,
    smp: Smp | None = None,
    kvm: bool = False,
    gdb: str | None = None,
) -> list[str]:
    """One machine plus one mode plus this run's paths -> a QEMU argv"""
    if mode_name not in machine.modes:
        known = ", ".join(sorted(machine.modes))
        raise MachineError(
            f"profile {machine.name!r} has no mode {mode_name!r} ({known})"
        )
    mode = machine.modes[mode_name]

    memory_mib = machine.memory_mib if memory_mib is None else memory_mib
    smp = machine.smp if smp is None else smp

    argv = [machine.qemu]
    if machine.nodefaults:
        argv += ["-nodefaults"]
    argv += [
        "-cdrom", str(iso),
        "-boot", "d",
        "-m", f"{memory_mib}M",
        "-smp", smp.topo(),
        "-M", machine.type,
        "-qmp", f"unix:{qmp_socket},server,nowait",
        "-monitor", "none",
    ]  # fmt: skip
    if kvm:
        argv += ["-enable-kvm", "-cpu", "host"]

    argv += _numa(machine, memory_mib, smp)
    argv += _devices(machine, disk)

    if machine.trace.get("enabled") and trace_log is not None:
        for event in machine.trace.get("events", []):
            argv += ["-d", f"trace:{event}"]
        argv += ["-trace", f"file={trace_log}"]

    if machine.acpi.get("enabled") and acpi_dir is not None:
        # Load what is on disk; a hardcoded table list only ever disagrees.
        for table in sorted(Path(acpi_dir).glob("*.dat")):
            argv += ["-acpitable", f"file={table}"]

    argv += _mode_wiring(mode, machine_log, gdb)
    return argv


def _cmd_list(_args: argparse.Namespace) -> None:
    directory = machines_dir()
    for path in sorted(directory.glob("*.toml")):
        m = load(path.stem, directory=directory)
        modes = ", ".join(sorted(m.modes))
        print(f"{m.name:<12} {m.arch:<8} {m.type:<14} qemu>={m.min_qemu}  [{modes}]")


def _cmd_resolve(args: argparse.Namespace) -> None:
    print(resolve_machine_type(load(args.profile)))


def _cmd_render(args: argparse.Namespace) -> None:
    m = load(args.profile)
    if args.check_version:
        check_qemu_version(m)
    smp = None
    if args.smp:
        fields = dict(
            part.split("=", 1) for part in args.smp.replace(" ", "").split(",")
        )
        smp = Smp(**{k: int(v) for k, v in fields.items()})
    argv = render(
        m,
        args.mode,
        iso=args.iso,
        disk=args.disk,
        qmp_socket=args.qmp_socket,
        machine_log=args.machine_log,
        trace_log=args.trace_log,
        acpi_dir=args.acpi_dir,
        memory_mib=parse_memory(args.memory) if args.memory else None,
        smp=smp,
        kvm=args.kvm,
        gdb="wait" if args.gdb_wait else None,
    )
    text = "\n".join(argv) + "\n"
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


def main(argv: Sequence[str] | None = None) -> int:
    """`charm machine`, stdlib-only so CMake can run it at configure time"""
    ap = argparse.ArgumentParser(
        prog="charm machine", description="the guest QEMU boots"
    )
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="every profile and the modes it defines").set_defaults(
        fn=_cmd_list
    )

    rv = sub.add_parser("resolve", help="what the profile's machine type means here")
    rv.add_argument("--profile", default="default")
    rv.set_defaults(fn=_cmd_resolve)

    rd = sub.add_parser("render", help="one profile and mode -> a QEMU argv")
    rd.add_argument("--profile", default="default")
    rd.add_argument("--mode", required=True)
    rd.add_argument("--iso", required=True)
    rd.add_argument("--disk")
    rd.add_argument("--qmp-socket", required=True)
    rd.add_argument("--machine-log")
    rd.add_argument("--trace-log")
    rd.add_argument("--acpi-dir", type=Path)
    rd.add_argument("--memory", help="override, e.g. 4G")
    rd.add_argument("--smp", help="override, e.g. sockets=1,cores=2,threads=1")
    rd.add_argument("--kvm", action="store_true")
    rd.add_argument("--gdb-wait", action="store_true", help="halt at startup (-S)")
    rd.add_argument(
        "--check-version", action="store_true", help="refuse a QEMU below min_qemu"
    )
    rd.add_argument("--out", help="write here instead of stdout")
    rd.set_defaults(fn=_cmd_render)

    args = ap.parse_args(argv)
    try:
        args.fn(args)
    except MachineError as error:
        print(f"charm machine: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
