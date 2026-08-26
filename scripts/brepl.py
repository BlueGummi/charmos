#!/usr/bin/env python3
"""
brepl.py - Interactive C BIT_RANGE macro definition generator

Usage:
    python3 brepl.py                  # interactive REPL
    python3 brepl.py -f input.txt     # seed from file, then REPL
    python3 brepl.py -f input.txt -q  # file-only, no REPL (just dump)

Input file format (one definition per line, comments with #):
    # register name for grouping
    CAP_ND   0  2
    CAP_RWBF 4
    GCMD_TE  31
    FSTS_FRI 8 15

Prefix a name with ! to emit a plain constant instead of a function-like macro:
    !MY_FLAG 4      -> #define MY_FLAG BIT(4)
    !MY_MASK 4 7    -> #define MY_MASK 0xf0
"""

import argparse
import re
import readline  # noqa: F401
import subprocess
import sys
import textwrap

USE_COLOR = hasattr(sys.stdout, "isatty") and sys.stdout.isatty()

class C:
    RESET   = "\033[0m"
    BOLD    = "\033[1m"
    GREEN   = "\033[92m"
    YELLOW  = "\033[93m"
    CYAN    = "\033[96m"
    RED     = "\033[91m"
    BLUE    = "\033[94m"
    MAGENTA = "\033[95m"
    WHITE   = "\033[97m"
    GREY    = "\033[90m"

def c(code: str, text: str) -> str:
    return f"{code}{text}{C.RESET}" if USE_COLOR else text

def ok(s):    return c(C.GREEN,          s)
def err(s):   return c(C.RED,            s)
def warn(s):  return c(C.YELLOW,         s)
def dim(s):   return c(C.GREY,           s)
def hi(s):    return c(C.CYAN,           s)
def macro_c(s): return c(C.GREEN,        s)
def const_c(s): return c(C.BLUE + C.BOLD, s)
def sec_c(s):   return c(C.YELLOW,       s)
def pfx_c(s):   return c(C.MAGENTA,      s)
def num_c(s):   return c(C.CYAN + C.BOLD, s)
def kw(s):      return c(C.BOLD + C.WHITE, s)

def _try_copy_to_clipboard(text: str) -> bool:
    for cmd in [["pbcopy"], ["xclip", "-selection", "clipboard"], ["wl-copy"]]:
        try:
            r = subprocess.run(cmd, input=text.encode(), timeout=2, capture_output=True)
            if r.returncode == 0:
                return True
        except Exception:
            pass
    try:
        import pyperclip
        pyperclip.copy(text)
        return True
    except Exception:
        pass
    return False

def make_macro(name: str, lo: int, hi: int, prefix: str = "", constant: bool = False) -> str:
    full_name = f"{prefix}{name}"
    if constant:
        if lo == hi:
            return f"#define {full_name} BIT({lo})"
        else:
            mask = ((1 << (hi - lo + 1)) - 1) << lo
            return f"#define {full_name} {hex(mask)}"
    body = f"BIT_TEST((arg), {lo})" if lo == hi else f"BIT_RANGE((arg), {lo}, {hi})"
    return f"#define {full_name}(arg) {body}"

class Entry:
    __slots__ = ("constant", "hi", "lo", "name", "prefix", "section")
    def __init__(self, section: str, name: str, lo: int, hi: int, prefix: str, constant: bool = False):
        self.section  = section
        self.name     = name
        self.lo       = lo
        self.hi       = hi
        self.prefix   = prefix
        self.constant = constant

    @property
    def macro(self) -> str:
        return make_macro(self.name, self.lo, self.hi, self.prefix, self.constant)

class Session:
    def __init__(self):
        self._entries: list[Entry] = []
        self._current_section: str = ""
        self.prefix: str = ""

    def set_section(self, comment: str):
        self._current_section = comment

    def add(self, name: str, lo: int, hi: int, constant: bool = False) -> Entry:
        e = Entry(self._current_section, name, lo, hi, self.prefix, constant)
        self._entries.append(e)
        return e

    def delete(self, idx: int) -> Entry | None:
        if 1 <= idx <= len(self._entries):
            return self._entries.pop(idx - 1)
        return None

    def delete_range(self, lo: int, hi: int) -> list[Entry]:
        lo = max(1, lo)
        hi = min(hi, len(self._entries))
        if lo > hi:
            return []
        removed = self._entries[lo - 1 : hi]
        del self._entries[lo - 1 : hi]
        return removed

    def undo(self) -> Entry | None:
        return self._entries.pop() if self._entries else None

    def all_macros(self) -> str:
        if not self._entries:
            return ""
        lines: list[str] = []
        last_section: str | None = None
        for e in self._entries:
            if e.section != last_section:
                if lines:
                    lines.append("")
                if e.section:
                    lines.append(f"/* {e.section} */")
                last_section = e.section
            lines.append(e.macro)
        return "\n".join(lines)

    def is_empty(self) -> bool:
        return not self._entries

    def clear(self):
        self._entries.clear()
        self._current_section = ""

    @property
    def entries(self) -> list[Entry]:
        return self._entries

def parse_definition_line(line: str) -> tuple[str, int, int, bool] | None:
    """Return (name, lo, hi, constant) or None."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None

    constant = line.startswith("!")
    if constant:
        line = line[1:].strip()

    parts = line.split()
    if len(parts) == 2:
        name, span = parts
        m = re.match(r"^(\d+)[:\-](\d+)$", span)
        if m:
            lo, hi = int(m.group(1)), int(m.group(2))
        else:
            try:
                lo = hi = int(span)
            except ValueError:
                return None
    elif len(parts) == 3:
        name, lo_s, hi_s = parts
        try:
            lo, hi = int(lo_s), int(hi_s)
        except ValueError:
            return None
    else:
        return None
    if lo > hi:
        lo, hi = hi, lo
    return name, lo, hi, constant

def load_file(path: str, session: Session) -> tuple[int, list[str]]:
    count, errors = 0, []
    try:
        with open(path) as f:
            for lineno, raw in enumerate(f, 1):
                raw = raw.rstrip()
                if not raw:
                    continue
                if raw.lstrip().startswith("#"):
                    session.set_section(raw.lstrip().lstrip("#").strip())
                    continue
                result = parse_definition_line(raw)
                if result is None:
                    errors.append(f"line {lineno}: cannot parse '{raw}'")
                    continue
                name, lo, hi, constant = result
                session.add(name, lo, hi, constant)
                count += 1
    except OSError as e:
        errors.append(str(e))
    return count, errors

def print_numbered_list(session: Session):
    if session.is_empty():
        print(dim("  (nothing yet)"))
        return
    entries = session.entries
    w = len(str(len(entries)))
    last_section: str | None = None
    for i, e in enumerate(entries, 1):
        if e.section != last_section:
            if last_section is not None:
                print()
            if e.section:
                print(sec_c(f"  /* {e.section} */"))
            last_section = e.section
        colour = const_c if e.constant else macro_c
        print(f"  {num_c(f'{i:>{w}}.')} {colour(e.macro)}")

def build_help() -> str:
    B = dim
    return "\n".join([
        B("  ┌─ Entry ────────────────────────────────────────────────────────┐"),
        B("  │ ") + kw("NAME lo hi") + "          multi-bit macro: BIT_RANGE(arg, lo, hi)    " + B("│"),
        B("  │ ") + kw("NAME lo") + "             single-bit macro: BIT_TEST(arg, lo)        " + B("│"),
        B("  │ ") + kw("NAME lo:hi") + "          colon or dash range also OK                " + B("│"),
        B("  ├─ Constant mode (!-prefix) ─────────────────────────────────────┤"),
        B("  │ ") + kw("!NAME lo") + "            plain constant:  #define NAME BIT(lo)      " + B("│"),
        B("  │ ") + kw("!NAME lo hi") + "         plain constant:  #define NAME 0x<mask>     " + B("│"),
        B("  ├─ Prefix ───────────────────────────────────────────────────────┤"),
        B("  │ ") + kw("prefix CAP_") + "         prefix all new macros: CAP_ND, CAP_RWBF    " + B("│"),
        B("  │ ") + kw("prefix") + "              clear prefix                               " + B("│"),
        B("  ├─ Sections ─────────────────────────────────────────────────────┤"),
        B("  │ ") + kw("section <text>") + "      group following macros under a comment     " + B("│"),
        B("  │ ") + kw("section") + "             clear current section                      " + B("│"),
        B("  ├─ Viewing / editing ────────────────────────────────────────────┤"),
        B("  │ ") + kw("show") + "                numbered list of all macros so far         " + B("│"),
        B("  │ ") + kw("del 3") + "               delete entry #3                            " + B("│"),
        B("  │ ") + kw("del 2-5") + "             delete entries #2 through #5               " + B("│"),
        B("  │ ") + kw("undo") + "                remove last entry                          " + B("│"),
        B("  │ ") + kw("rename 3 NEW_NAME") + "   rename entry #3 (keeps prefix/section)     " + B("│"),
        B("  │ ") + kw("move 3 1") + "            reorder: move entry #3 to position #1      " + B("│"),
        B("  │ ") + kw("clear") + "               wipe entire session                        " + B("│"),
        B("  ├─ Output ───────────────────────────────────────────────────────┤"),
        B("  │ ") + kw("show") + "                print numbered list                        " + B("│"),
        B("  │ ") + kw("copy") + "                copy all macros to clipboard               " + B("│"),
        B("  │ ") + kw("save <file>") + "         write macros to a file                     " + B("│"),
        B("  │ ") + kw("load <file>") + "         import definitions from a file             " + B("│"),
        B("  ├────────────────────────────────────────────────────────────────┤"),
        B("  │ ") + kw("help / ?") + "            show this message                          " + B("│"),
        B("  │ ") + kw("quit / exit / ^D") + "    print final output and exit                " + B("│"),
        B("  └────────────────────────────────────────────────────────────────┘"),
    ])

BANNER = "\n".join([
    c(C.BLUE + C.BOLD, "  ╔══════════════════════════════════════════╗"),
    c(C.BLUE + C.BOLD, "  ║") + "  " + c(C.WHITE + C.BOLD, "BIT_RANGE C macro generator") + "             " + c(C.BLUE + C.BOLD, "║"),
    c(C.BLUE + C.BOLD, "  ║") + "  " + dim("type help for commands, ^D to exit ") + "     " + c(C.BLUE + C.BOLD, "║"),
    c(C.BLUE + C.BOLD, "  ╚══════════════════════════════════════════╝"),
])

def prompt_str(session: Session) -> str:
    pfx = (c(C.MAGENTA, "[") + pfx_c(session.prefix) + c(C.MAGENTA, "]")) if session.prefix else ""
    return hi("bit") + pfx + hi("> ")

def run_repl(session: Session, quiet_start: bool = False):
    if not quiet_start:
        print(BANNER)
        print()

    while True:
        try:
            raw = input(prompt_str(session)).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not raw:
            continue

        tokens = raw.split(None, 1)
        cmd = tokens[0].lower()
        rest = tokens[1].strip() if len(tokens) > 1 else ""

        if cmd in ("quit", "exit", "q"):
            break

        if cmd in ("help", "?"):
            print(build_help())
            continue

        if cmd == "show":
            print()
            print_numbered_list(session)
            print()
            continue

        if cmd == "copy":
            text = session.all_macros()
            if not text:
                print(warn("  nothing to copy."))
            elif _try_copy_to_clipboard(text):
                print(ok("  ✓ copied to clipboard."))
            else:
                print(err("  ✗ clipboard unavailable (pbcopy / xclip / wl-copy / pyperclip)."))
            continue

        if cmd == "undo":
            e = session.undo()
            print(warn(f"  undo: {e.macro}") if e else dim("  nothing to undo."))
            continue

        if cmd in ("del", "delete", "rm"):
            if not rest:
                print(err("  usage: del <n>  or  del <lo>-<hi>"))
                continue
            m = re.match(r"^(\d+)[:\-](\d+)$", rest)
            if m:
                lo_i, hi_i = int(m.group(1)), int(m.group(2))
                if lo_i > hi_i:
                    lo_i, hi_i = hi_i, lo_i
                removed = session.delete_range(lo_i, hi_i)
                if removed:
                    print(warn(f"  deleted {len(removed)} entr{'y' if len(removed)==1 else 'ies'} ({lo_i}–{hi_i})."))
                else:
                    print(err(f"  no entries in range {lo_i}–{hi_i}."))
            else:
                try:
                    idx = int(rest)
                except ValueError:
                    print(err(f"  '{rest}' is not a valid number or range."))
                    continue
                e = session.delete(idx)
                if e:
                    print(warn(f"  deleted [{idx}]: {e.macro}"))
                else:
                    print(err(f"  no entry #{idx}  (session has {len(session.entries)})."))
            continue

        if cmd == "clear":
            n = len(session.entries)
            session.clear()
            print(warn(f"  cleared {n} entr{'y' if n == 1 else 'ies'}."))
            continue

        if cmd == "section":
            session.set_section(rest)
            print(sec_c(f"  section → '{rest}'") if rest else dim("  section cleared."))
            continue

        if cmd == "prefix":
            old = session.prefix
            session.prefix = rest
            if old and rest:
                print(pfx_c(f"  prefix changed from '{old}' to '{rest}'"))
            elif rest:
                print(pfx_c(f"  prefix set to '{rest}'"))
            else:
                print(dim("  prefix cleared."))
            continue

        if cmd == "rename":
            parts = rest.split()
            if len(parts) != 2:
                print(err("  usage: rename <n> <NEW_NAME>"))
                continue
            try:
                idx = int(parts[0])
            except ValueError:
                print(err(f"  '{parts[0]}' is not a number."))
                continue
            if not (1 <= idx <= len(session.entries)):
                print(err(f"  no entry #{idx}."))
                continue
            e = session.entries[idx - 1]
            old_macro = e.macro
            e.name = parts[1]
            print(ok(f"  [{idx}] {old_macro}"))
            print(ok(f"    → {e.macro}"))
            continue

        if cmd == "move":
            parts = rest.split()
            if len(parts) != 2:
                print(err("  usage: move <from> <to>"))
                continue
            try:
                src, dst = int(parts[0]), int(parts[1])
            except ValueError:
                print(err("  both arguments must be numbers."))
                continue
            n = len(session.entries)
            if not (1 <= src <= n and 1 <= dst <= n):
                print(err(f"  indices out of range (session has {n} entries)."))
                continue
            if src == dst:
                print(dim("  nothing to do."))
                continue
            e = session.entries.pop(src - 1)
            session.entries.insert(dst - 1, e)
            print(ok(f"  moved [{src}] → [{dst}]: {e.macro}"))
            continue

        if cmd == "load":
            if not rest:
                print(err("  usage: load <filepath>"))
                continue
            n, errs = load_file(rest, session)
            print(ok(f"  loaded {n} definition(s) from '{rest}'."))
            for e in errs:
                print(err(f"  ⚠  {e}"))
            continue

        if cmd == "save":
            if not rest:
                print(err("  usage: save <filepath>"))
                continue
            text = session.all_macros()
            if not text:
                print(warn("  nothing to save."))
                continue
            try:
                with open(rest, "w") as f:
                    f.write(text + "\n")
                print(ok(f"  saved to '{rest}'."))
            except OSError as ex:
                print(err(f"  ✗ {ex}"))
            continue

        # ── definition entry ──────────────────────────────────────────────────
        result = parse_definition_line(raw)
        if result:
            name, lo, hi, constant = result
            entry = session.add(name, lo, hi, constant)
            n = len(session.entries)
            w = len(str(n))
            colour = const_c if constant else macro_c
            print(f"  {num_c(f'{n:>{w}}.')} {colour(entry.macro)}")
        else:
            print(err(f"  ✗ unrecognised: '{raw}'  — type 'help'."))

def print_summary(session: Session):
    if session.is_empty():
        print(dim("\n(no definitions generated)"))
        return

    output = session.all_macros()
    bar = c(C.BLUE, "═" * 72)
    print(f"\n{bar}")
    print(c(C.BOLD + C.WHITE, "  GENERATED MACROS"))
    print(bar)
    for line in output.splitlines():
        if not line:
            print()
        elif line.startswith("/*"):
            print(sec_c(line))
        else:
            print(macro_c(line))
    print(bar)

    try:
        ans = input(hi("\nCopy to clipboard? [Y/n] ")).strip().lower()
    except (EOFError, KeyboardInterrupt):
        ans = "n"

    if ans in ("", "y", "yes"):
        print(ok("✓ copied.") if _try_copy_to_clipboard(output) else err("✗ no clipboard backend found."))

def main():
    parser = argparse.ArgumentParser(
        description="Interactive C BIT_RANGE macro generator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            File format:
              # Section comment
              CAP_ND   0  2
              GCMD_TE  31
              FSTS_FRI 8:15

            Prefix a name with ! for a plain constant instead of a function-like macro:
              !CAP_ND   0  2    ->  #define CAP_ND 0x7
              !GCMD_TE  31      ->  #define GCMD_TE BIT(31)
        """),
    )
    parser.add_argument("-f", "--file", metavar="FILE",
                        help="seed definitions from a file before opening the REPL")
    parser.add_argument("-q", "--quiet", action="store_true",
                        help="file-only: load, dump, exit (no REPL)")
    parser.add_argument("-c", "--copy", action="store_true",
                        help="auto-copy output to clipboard on exit")
    args = parser.parse_args()

    session = Session()

    if args.file:
        n, errs = load_file(args.file, session)
        print(ok(f"Loaded {n} definition(s) from '{args.file}'."))
        for e in errs:
            print(warn(f"  ⚠  {e}"))

    if args.quiet:
        output = session.all_macros()
        if output:
            print(output)
            if args.copy:
                print("# ✓ copied" if _try_copy_to_clipboard(output) else "# ✗ clipboard unavailable")
        return

    run_repl(session, quiet_start=bool(args.file))

    if args.copy:
        output = session.all_macros()
        if output:
            bar = "═" * 72
            print(f"\n{bar}\n{output}\n{bar}")
            print(ok("✓ copied.") if _try_copy_to_clipboard(output) else err("✗ clipboard unavailable."))
    else:
        print_summary(session)


if __name__ == "__main__":
    main()
