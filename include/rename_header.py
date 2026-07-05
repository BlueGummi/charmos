#!/usr/bin/env python3
"""
rename_header.py — resolution-aware header renamer.

Unlike a basename match, this resolves every #include the way the compiler
would (using the -I search paths from compile_commands.json, honoring the
<> vs "" distinction) and only rewrites includes that ACTUALLY resolve to the
file being moved. So limine.c's <time.h> (libc) is left untouched even while
your project's time.h -> time/time.h is being moved.

Usage:
    python3 include/rename_header.py <old_path> <new_path> [--dry-run] [--no-git]

Paths are the header locations on disk (e.g. include/time.h include/time/time.h).
Run from anywhere; paths are resolved relative to the current directory.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

INCLUDE_RE = re.compile(r'^([ \t]*#[ \t]*include[ \t]*)([<"])([^">]+)([>"])(.*)$')
# Translation units: only rewritten if they appear in compile_commands.json,
# i.e. they are actually part of THIS build. A .c not in the DB (e.g. the
# host-built limine.c) is a foreign TU with its own search paths -> skip it.
TU_EXTS = ('.c', '.cpp', '.cc', '.cxx')
# Headers aren't compiled directly / aren't in the DB; rewrite them using the
# union of search paths (best effort), resolving "" against their own dir.
HDR_EXTS = ('.h', '.hpp', '.hh', '.hxx', '.inl', '.ipp')
SRC_EXTS = TU_EXTS + HDR_EXTS


def find_repo_root(start):
    d = start
    while True:
        if os.path.isdir(os.path.join(d, '.git')):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return start  # not a git repo; fall back to start
        d = parent


def load_compile_db(repo_root):
    """Return {abs_file: [abs_include_dir, ...]} from compile_commands.json."""
    for cand in ('compile_commands.json', 'build/compile_commands.json'):
        path = os.path.join(repo_root, cand)
        if os.path.isfile(path):
            break
    else:
        return {}, []

    with open(path) as f:
        db = json.load(f)

    per_file = {}
    all_dirs = []
    seen = set()
    for entry in db:
        cwd = entry.get('directory', repo_root)
        argv = entry.get('arguments')
        if argv is None:
            argv = entry.get('command', '').split()
        dirs = []
        i = 0
        while i < len(argv):
            a = argv[i]
            if a == '-I':
                i += 1
                if i < len(argv):
                    dirs.append(os.path.normpath(os.path.join(cwd, argv[i])))
            elif a.startswith('-I'):
                dirs.append(os.path.normpath(os.path.join(cwd, a[2:])))
            i += 1
        f_abs = os.path.normpath(os.path.join(cwd, entry['file']))
        per_file[f_abs] = dirs
        for d in dirs:
            if d not in seen:
                seen.add(d)
                all_dirs.append(d)
    return per_file, all_dirs


def resolve_include(spell, angle, including_file, search_dirs):
    """Return the abs path this include resolves to, or None."""
    candidates = []
    if not angle:  # "" include: including file's own dir first
        candidates.append(os.path.dirname(including_file))
    candidates.extend(search_dirs)
    for base in candidates:
        cand = os.path.normpath(os.path.join(base, spell))
        if os.path.isfile(cand):
            return cand
    return None


def spelling_for(new_abs, angle, including_file, search_dirs):
    """Compute the new include spelling as seen from `including_file`.

    Keep it resolvable under the same rule the old one used. Prefer a search
    dir (stable, path-independent); for "" includes fall back to a path
    relative to the including file if no search dir covers it.
    """
    for base in search_dirs:
        rel = os.path.relpath(new_abs, base)
        if not rel.startswith('..') and not os.path.isabs(rel):
            return rel.replace(os.sep, '/')
    if not angle:
        rel = os.path.relpath(new_abs, os.path.dirname(including_file))
        return rel.replace(os.sep, '/')
    return None


def iter_source_files(repo_root):
    for root, dirs, files in os.walk(repo_root):
        dirs[:] = [d for d in dirs if d not in ('.git', 'build')]
        for fn in files:
            if fn.endswith(SRC_EXTS):
                yield os.path.join(root, fn)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old_path')
    ap.add_argument('new_path')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--no-git', action='store_true',
                    help='use shutil.move instead of git mv')
    args = ap.parse_args()

    old_abs = os.path.abspath(args.old_path)
    new_abs = os.path.abspath(args.new_path)
    if not os.path.isfile(old_abs):
        sys.exit(f'error: {old_abs} is not a file')

    repo_root = find_repo_root(os.path.dirname(old_abs))
    per_file, all_dirs = load_compile_db(repo_root)
    if not all_dirs:
        print('warning: no compile_commands.json found; falling back to '
              f'{repo_root}/include as the only search dir', file=sys.stderr)
        all_dirs = [os.path.join(repo_root, 'include')]

    # --- rewrite includers (resolve against the OLD location) ---
    edits = []  # (file, new_content)
    for src in iter_source_files(repo_root):
        norm = os.path.normpath(src)
        if src.endswith(TU_EXTS) and norm not in per_file:
            # foreign translation unit (not in this build) -> never touch
            continue

        try:
            with open(src, encoding='utf-8') as f:
                lines = f.readlines()
        except (UnicodeDecodeError, OSError):
            continue

        search_dirs = per_file.get(norm, all_dirs)
        changed = False
        for idx, line in enumerate(lines):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            pre, open_d, spell, close_d, rest = m.groups()
            angle = open_d == '<'
            resolved = resolve_include(spell, angle, src, search_dirs)
            if resolved != old_abs:
                continue
            new_spell = spelling_for(new_abs, angle, src, search_dirs)
            if new_spell is None:
                print(f'  ! {src}: cannot express new path from its search '
                      'paths; leaving as-is', file=sys.stderr)
                continue
            lines[idx] = f'{pre}{open_d}{new_spell}{close_d}{rest}\n'
            changed = True
        if changed:
            edits.append((src, ''.join(lines)))

    # --- report + apply ---
    action = 'DRY-RUN' if args.dry_run else 'move'
    print(f'[{action}] {old_abs} -> {new_abs}')
    for src, _ in edits:
        print(f'  update {os.path.relpath(src, repo_root)}')
    if not edits:
        print('  (no includers resolve to this header)')

    if args.dry_run:
        return

    new_dir = os.path.dirname(new_abs)
    if new_dir and not os.path.isdir(new_dir):
        os.makedirs(new_dir)
    if args.no_git or not os.path.isdir(os.path.join(repo_root, '.git')):
        shutil.move(old_abs, new_abs)
    else:
        subprocess.run(['git', 'mv', old_abs, new_abs], cwd=repo_root, check=True)

    for src, content in edits:
        with open(src, 'w', encoding='utf-8') as f:
            f.write(content)


if __name__ == '__main__':
    main()
