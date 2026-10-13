"""Assemble the bootable ISO: the kernel, Limine, and a limine.conf with the
kernel command line appended

    mkiso.py <kernel> <limine.conf> <limine-dir> <out.iso>

The command line comes from the environment; mutually exclusive, in precedence:

    CMDLINE=<path>          read <path>, append its contents
    NIGHTMARE_TESTS=<name>  shorthand, expands to nightmare=<name>
    TESTS=a,b,c             expands to test.filter=a,b,c

EXTRA_CMDLINE=<string> appends at the end
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Mapping
from pathlib import Path

CMDLINE_LINE_RE = re.compile(r"\n[ \t]*cmdline:[^\n]*")
PATH_LINE_RE = re.compile(r"\n[ \t]*path:[^\n]*")


LIMINE_BOOT_ASSETS = ("limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin")
EFI_ASSETS = ("BOOTX64.EFI", "BOOTIA32.EFI")
# fmt: off
XORRISO_FLAGS = (
    "-as", "mkisofs", "-R", "-r", "-J",
    "-b", "boot/limine/limine-bios-cd.bin",
    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
    "-hfsplus", "-apm-block-size", "2048",
    "--efi-boot", "boot/limine/limine-uefi-cd.bin",
    "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
)
# fmt: on


class ConfError(Exception):
    pass


def resolve_cmdline(env: Mapping[str, str] | None = None) -> str:
    source = os.environ if env is None else env

    def value(name: str) -> str:
        return source.get(name, "").strip()

    cmdline_file = value("CMDLINE")
    nightmare = value("NIGHTMARE_TESTS")
    tests = value("TESTS")

    chosen = [
        name
        for name, value in (
            ("CMDLINE", cmdline_file),
            ("NIGHTMARE_TESTS", nightmare),
            ("TESTS", tests),
        )
        if value
    ]
    if len(chosen) > 1:
        raise ConfError(f"{', '.join(chosen)} are set together")

    line = ""
    if cmdline_file:
        path = Path(cmdline_file)
        if not path.exists():
            raise ConfError(f"CMDLINE points at '{cmdline_file}', which does not exist")
        line = re.sub(r"[\r\n]+", " ", path.read_text(encoding="utf-8")).strip()
        if not line:
            raise ConfError(f"CMDLINE file '{cmdline_file}' is empty")

    elif nightmare:
        if re.search(r"[ \t,]", nightmare):
            raise ConfError(
                f"NIGHTMARE_TESTS takes exactly one test name (got '{nightmare}')"
            )
        line = f"nightmare={nightmare}"

    elif tests:
        normalized = re.sub(r"[ \t,]+", ",", tests)
        if normalized.startswith(",") or normalized.endswith(","):
            raise ConfError(
                f"TESTS list has a leading or trailing comma (got '{tests}')"
            )
        line = f"test.filter={normalized}"

    extra = value("EXTRA_CMDLINE")
    if extra:
        line = f"{line} {extra}".strip()
    return line


def render(conf: str, line: str) -> str:
    """Splice `line` into an existing cmdline"""
    if not line:
        return conf
    if CMDLINE_LINE_RE.search(conf):
        return CMDLINE_LINE_RE.sub(lambda m: f"{m.group(0)} {line}", conf)
    return PATH_LINE_RE.sub(lambda m: f"{m.group(0)}\n    cmdline: {line}", conf)


def _run(command: list[str]) -> None:
    done = subprocess.run(command, capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise ConfError(
            f"{Path(command[0]).name} failed with exit code {done.returncode}: "
            f"{done.stdout}{done.stderr}"
        )


def build(
    kernel: Path, conf: Path, limine: Path, output: Path, line: str, work: Path
) -> None:
    """`limine` holds the boot assets and the built `limine` installer"""
    root = work / "iso_root"
    shutil.rmtree(root, ignore_errors=True)
    (root / "boot" / "limine").mkdir(parents=True)
    (root / "EFI" / "BOOT").mkdir(parents=True)

    shutil.copyfile(kernel, root / "boot" / "kernel")
    (root / "boot" / "limine" / "limine.conf").write_text(
        render(conf.read_text(encoding="utf-8"), line), encoding="utf-8"
    )
    for asset in LIMINE_BOOT_ASSETS:
        shutil.copyfile(limine / asset, root / "boot" / "limine" / asset)
    for asset in EFI_ASSETS:
        shutil.copyfile(limine / asset, root / "EFI" / "BOOT" / asset)

    output.parent.mkdir(parents=True, exist_ok=True)
    _run(["xorriso", *XORRISO_FLAGS, str(root), "-o", str(output)])
    _run([str(limine / "limine"), "bios-install", str(output)])


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print(
            f"usage: {Path(argv[0]).name} <kernel> <limine.conf> <limine-dir> <out.iso>",
            file=sys.stderr,
        )
        return 2

    kernel, conf, limine, output = map(Path, argv[1:])
    try:
        with tempfile.TemporaryDirectory(prefix="mkiso-") as work:
            build(kernel, conf, limine, output, resolve_cmdline(), Path(work))
    except ConfError as error:
        print(f"mkiso: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"mkiso: {error.strerror or error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
