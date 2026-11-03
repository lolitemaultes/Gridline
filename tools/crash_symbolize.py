#!/usr/bin/env python3
"""Turn a Gridline crash code into functions and lines.

    tools/crash_symbolize.py code.txt --symbols <folder>     (or - for stdin)

The folder is an unpacked Gridline-<version>-symbols-<target>.zip from the release the code names
(version: and arch: are in the code; the zip for that version is on the releases page), or a build
folder of the same commit. Per module it looks for:
    <module>.dSYM      macOS       (atos, or llvm-symbolizer elsewhere)
    <module>.debug     Linux, BSD  (addr2line or llvm-symbolizer)
    <module>           unstripped  (same)
    <stem>.pdb         Windows     (llvm-symbolizer, with <module> beside it)

Prints the code with each frame it could resolve followed by its function and file:line. Frames that
end in " ?" came from scanning the stack and may be stale values.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

BEGIN = "GRIDLINE CRASH CODE 1"
END = "END GRIDLINE CRASH CODE"
FRAME = re.compile(r"^\s*(\d+) (.+?)\+0x([0-9a-f]+)( \?)?\s*$")


def extract(text):
    """The lines of the code from a paste: quote marks and fences are fine."""
    lines, inside = [], False
    for raw in text.splitlines():
        line = re.sub(r"^[>`\s]*(?=GRIDLINE|END GRIDLINE)", "", raw)
        line = re.sub(r"^>+ ?", "", line)
        if not inside and line.startswith(BEGIN):
            inside = True
        if inside:
            lines.append(line)
            if line.startswith(END):
                break
    return lines


def field(lines, key):
    for line in lines:
        if line.startswith(key + ": "):
            return line[len(key) + 2:].strip()
    return ""


def find_symbols(folder, module):
    dsym = os.path.join(folder, module + ".dSYM")
    if os.path.isdir(dsym):
        return "dsym", os.path.join(dsym, "Contents", "Resources", "DWARF", module)
    debug = os.path.join(folder, module + ".debug")
    if os.path.isfile(debug):
        return "elf", debug
    stem = os.path.splitext(module)[0]
    pdb = os.path.join(folder, stem + ".pdb")
    plain = os.path.join(folder, module)
    if os.path.isfile(pdb):
        return "pdb", plain if os.path.isfile(plain) else pdb
    if os.path.isfile(plain):
        return "elf", plain
    return None, None


def run(cmd, stdin=None):
    try:
        return subprocess.run(cmd, input=stdin, capture_output=True, text=True, check=False).stdout
    except OSError as e:
        sys.stderr.write(f"could not run {cmd[0]}: {e}\n")
        return ""


def pretty(lines):
    """Pairs of function, location per address from addr2line / llvm-symbolizer style output."""
    out, cur = [], []
    for line in lines.splitlines():
        if not line.strip():
            if cur:
                out.append(cur)
            cur = []
        else:
            cur.append(line.strip())
    if cur:
        out.append(cur)
    return out


def resolve(kind, path, arch, offsets, exact):
    """{offset: 'function file:line'}. A return address points after its call: look one byte back."""
    wanted = {o: (o if o in exact else max(o - 1, 0)) for o in offsets}
    result = {}
    if kind == "dsym" and shutil.which("atos"):
        base = 0x100000000  # atos works from any load address it is told; the code's offsets are from the image's start
        addrs = [hex(base + wanted[o]) for o in offsets]
        text = run(["atos", "-o", path, "-arch", arch, "-l", hex(base)] + addrs)
        for o, line in zip(offsets, text.splitlines()):
            if line.strip() and not line.startswith("0x"):
                result[o] = line.strip()
        return result
    tool = shutil.which("llvm-symbolizer")
    if tool:
        for o in offsets:
            cmd = [tool, "--obj=" + path, "--demangle"]
            if kind == "pdb":
                cmd.append("--relative-address")
            found = pretty(run(cmd + [hex(wanted[o])]))
            if found and found[0] and found[0][0] != "??":
                result[o] = " ".join(found[0][:2])
        return result
    tool = shutil.which("addr2line")
    if tool and kind != "pdb":
        for o in offsets:
            parts = run([tool, "-C", "-f", "-e", path, hex(wanted[o])]).split("\n")
            if parts and parts[0] and parts[0] != "??":
                result[o] = (parts[0] + " " + (parts[1] if len(parts) > 1 else "")).strip()
        return result
    sys.stderr.write(f"no tool for {path}: install llvm-symbolizer, addr2line or Xcode's atos\n")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("code", help="a file holding the code, or - for standard input")
    ap.add_argument("--symbols", required=True, help="folder with the symbol files of that release")
    args = ap.parse_args()
    text = sys.stdin.read() if args.code == "-" else open(args.code, encoding="utf-8", errors="replace").read()
    lines = extract(text)
    if not lines:
        sys.exit("no Gridline crash code in that text")
    arch = field(lines, "arch") or "x86_64"
    print(f"# Gridline {field(lines, 'version')} build {field(lines, 'build')} {arch}, {field(lines, 'os')}")
    per_module = {}
    frames = {}
    for i, line in enumerate(lines):
        m = FRAME.match(line)
        if m:
            module, off = m.group(2), int(m.group(3), 16)
            per_module.setdefault(module, []).append(off)
            frames[i] = (module, off, int(m.group(1)))
    names = {}
    missing = []
    for module, offs in per_module.items():
        kind, path = find_symbols(args.symbols, module)
        if not kind:
            missing.append(module)
            continue
        # The crashing frame (number 0) is an exact address; the others are return addresses.
        exact = {f[1] for f in frames.values() if f[0] == module and f[2] == 0}
        found = resolve(kind, path, arch, list(dict.fromkeys(offs)), exact)
        for o, name in found.items():
            names[(module, o)] = name
    for i, line in enumerate(lines):
        print(line)
        if i in frames:
            module, off, _ = frames[i]
            name = names.get((module, off))
            if name:
                print(f"       {name}")
    system = ("libsystem", "libc++", "libdyld", "dyld", "ntdll", "kernel", "libc.", "libpthread", "libstdc++", "AppKit", "CoreFoundation")
    ours = [m for m in missing if not m.startswith(system)]
    if ours:
        sys.stderr.write("no symbols for: " + ", ".join(ours) + "\n")


if __name__ == "__main__":
    main()
