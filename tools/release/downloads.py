#!/usr/bin/env python3
"""Gridline release downloads: the machine list and the release page's download sections.

    downloads.py list  <files dir> --version V [--sums SHA256SUMS] --out Gridline-V-downloads.json
    downloads.py body  <downloads.json> [--notes notes.md] --out notes-with-downloads.md
    downloads.py --self-test

`list` reads every release file name (never the files' contents, except their size and the
checksum from SHA256SUMS) and says what each one is: product (app, suite, mini, extras), kind
(installer, all, single, app, portable, ...), system family, processor and, for a single plugin,
its name. `body` writes the release page's four grouped sections in the approved order, each
opened by a one-line note:

  1. Gridline (the app): one main download per system;
  2. Gridline Suite: the installer, the all-plugins zip and the single-plugin zips per system;
  3. Gridline Suite Mini: the same three ways;
  4. Extras: media tools, crash symbols, source and checksums;

then the release notes. Names follow the plan's asset table; a name it cannot place fails the run,
but it is still listed (under Extras) with a warning, so a file is never left off the page. Public text only: no task numbers, no other products.
"""
import argparse
import json
import os
import re
import sys

REPO = "https://github.com/lolitemaultes/Gridline/releases/download"

FAMILIES = ("macos", "windows", "linux", "bsd")
FAMILY_NAME = {"macos": "macOS", "windows": "Windows", "linux": "Linux", "bsd": "BSD"}
FLAGSHIPS = ["Plasma", "Seltmann", "Platter", "Tideline", "Chater", "Loupe"]

MAC_NOTE = ("The macOS files are not signed by Apple. The first time, right-click (or Control-click) the "
            "file and choose Open, or open System Settings > Privacy & Security and click Open Anyway.")


def _family_of(system):
    if system.startswith("macos"):
        return "macos"
    if system.startswith("windows"):
        return "windows"
    if system.startswith("linux"):
        return "linux"
    if system.split("-")[0] in ("freebsd", "openbsd", "netbsd", "bsd"):
        return "bsd"
    return ""


def classify(name, version):
    """What a release file is, or None when the name is not one of ours."""
    v = re.escape(version)
    rules = [
        # ---- the plugins: Suite and Suite Mini
        (r"Gridline-Suite(-Mini)?-%s-(macos-universal)\.pkg" % v, "installer"),
        (r"Gridline-Suite(-Mini)?-%s-(windows)-setup\.exe" % v, "installer"),
        (r"Gridline-Suite(-Mini)?-%s-((?:linux|freebsd|openbsd|netbsd)-[a-z0-9_]+)-setup\.run" % v, "installer"),
        (r"Gridline-Suite(-Mini)?-%s-(macos|windows|linux|bsd)-all\.zip" % v, "all"),
        (r"Gridline-Suite(-Mini)?-([A-Za-z0-9]+)-%s-(macos|windows|linux|bsd)\.zip" % v, "single"),
        # ---- extras
        (r"Gridline-%s-media-tools-([a-z0-9_-]+)\.zip" % v, "media-tools"),
        (r"Gridline-%s-symbols-([a-z0-9_-]+)\.zip" % v, "symbols"),
        (r"Gridline-%s-source\.tar\.gz" % v, "source"),
        (r"Gridline-%s-SHA256SUMS" % v, "checksums"),
        (r"Gridline-%s-downloads\.json" % v, "list"),
        # ---- the app
        (r"Gridline-%s-(macos-universal)\.dmg" % v, "app"),
        (r"Gridline-%s-(windows-(?:x64|arm64))-setup\.exe" % v, "app"),
        (r"Gridline-%s-(windows-(?:x64|arm64))\.zip" % v, "portable"),
        (r"Gridline-%s-(linux-[a-z0-9_]+)\.(AppImage|deb|rpm|tar\.gz)" % v, "app"),
        (r"Gridline-%s-((?:freebsd|openbsd|netbsd)-[a-z0-9_]+)\.tar\.gz" % v, "app"),
    ]
    for pattern, kind in rules:
        m = re.fullmatch(pattern, name)
        if not m:
            continue
        g = m.groups()
        e = {"name": name, "kind": kind}
        if name.startswith("Gridline-Suite"):
            e["product"] = "mini" if g[0] else "suite"
            if kind == "single":
                e["plugin"], e["family"] = g[1], g[2]
            else:
                e["system"] = g[1]
                e["family"] = _family_of(g[1]) or g[1]
        elif kind in ("app", "portable"):
            e["product"] = "app"
            e["system"] = g[0]
            e["family"] = _family_of(g[0])
            e["format"] = g[1] if len(g) > 1 else name.rsplit(".", 1)[-1]
            if name.endswith(".tar.gz"):
                e["format"] = "tar.gz"
        else:
            e["product"] = "extras"
            if g:
                e["target"] = g[0]
        return e
    return None


def build_list(files_dir, version, sums_path=None):
    sums = {}
    if sums_path and os.path.exists(sums_path):
        with open(sums_path, encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    sums[parts[1].lstrip("*")] = parts[0]
    out, unknown = [], []
    for name in sorted(os.listdir(files_dir)):
        e = classify(name, version)
        if e is None:  # listed under Extras, and named in the log so the rules can learn it
            unknown.append(name)
            e = {"name": name, "kind": "other", "product": "extras"}
        e["size"] = os.path.getsize(os.path.join(files_dir, name))
        if name in sums:
            e["sha256"] = sums[name]
        out.append(e)
    for name in unknown:
        print("::warning::downloads.py does not know the release file %s; it is listed under Extras" % name)
    return {"version": version, "files": out}


def _link(version, name, text=None):
    return "[%s](%s/v%s/%s)" % (text or name, REPO, version, name)


def _app_section(version, files):
    apps = [f for f in files if f["product"] == "app"]
    if not apps:
        return []
    rows = []
    order = [("macos", "macOS"), ("windows", "Windows"), ("linux", "Linux"), ("bsd", "FreeBSD, OpenBSD, NetBSD")]
    for fam, label in order:
        main = [f for f in apps if f["family"] == fam and f["kind"] == "app"]
        if main:
            rows.append("| %s | %s |" % (label, ", ".join(_link(version, f["name"]) for f in main)))
    lines = ["## Gridline", "", "Gridline itself, with every Gridline plugin included.", "", "| System | Download |", "| --- | --- |"]
    lines += rows
    portable = [f for f in apps if f["kind"] == "portable"]
    if portable:
        lines += ["", "More: portable Windows zips, no installer: " + ", ".join(_link(version, f["name"]) for f in portable) + "."]
    return lines + [""]


def _plugin_order(names):
    flag = [n for n in FLAGSHIPS if n in names]
    rest = sorted(n for n in names if n not in FLAGSHIPS)
    return flag + rest


def _suite_section(version, files, product):
    title = "Gridline Suite" if product == "suite" else "Gridline Suite Mini"
    note = ("Only the plugins, for other music programs." if product == "suite"
            else "The small effects set, for other music programs.")
    mine = [f for f in files if f["product"] == product]
    if not mine:
        return []
    lines = ["## " + title, "", note, ""]
    inst = [f for f in mine if f["kind"] == "installer"]
    if inst:
        lines += ["**Installer**: one file per system; tick the plugins and the formats you want.", ""]
        for fam in FAMILIES:
            these = [f for f in inst if f["family"] == fam]
            if these:
                lines.append("- %s: %s" % (FAMILY_NAME[fam], ", ".join(_link(version, f["name"]) for f in these)))
        lines.append("")
    alls = [f for f in mine if f["kind"] == "all"]
    if alls:
        lines += ["**All plugins**: every plugin in every format, no installer (a README inside says where each folder goes).", ""]
        lines.append("- " + ", ".join(_link(version, f["name"], FAMILY_NAME[f["family"]]) for fam in FAMILIES for f in alls
                                       if f["family"] == fam))
        lines.append("")
    singles = [f for f in mine if f["kind"] == "single"]
    if singles:
        lines += ["**Single plugins**: one zip per plugin per system, with every format inside. Each is about as large as the "
                  "whole set, so for several plugins the installer or the all-plugins zip is smaller.", "",
                  "| Plugin | macOS | Windows | Linux | BSD |", "| --- | --- | --- | --- | --- |"]
        by = {}
        for f in singles:
            by.setdefault(f["plugin"], {})[f["family"]] = f["name"]
        for p in _plugin_order(list(by)):
            cells = [_link(version, by[p][fam], "zip") if fam in by[p] else "" for fam in FAMILIES]
            lines.append("| %s | %s |" % (p, " | ".join(cells)))
        lines.append("")
    return lines


def _extras_section(version, files):
    ex = [f for f in files if f["product"] == "extras"]
    if not ex:
        return []
    labels = {"media-tools": "Media tools", "symbols": "Crash symbol files", "source": "Source code",
              "checksums": "Checksums", "list": "List of every file (for scripts and package channels)", "other": "Other files"}
    lines = ["## Extras", "", "Not needed to make music.", ""]
    for kind in ("media-tools", "symbols", "source", "checksums", "list", "other"):
        these = [f for f in ex if f["kind"] == kind]
        if these:
            lines.append("- %s: %s" % (labels[kind], ", ".join(_link(version, f["name"]) for f in these)))
    return lines + [""]


def render_body(data, notes=""):
    version, files = data["version"], data["files"]
    lines = ["# Gridline %s" % version, ""]
    if any(f.get("family") == "macos" for f in files):
        lines += [MAC_NOTE, ""]
    lines += _app_section(version, files)
    lines += _suite_section(version, files, "suite")
    lines += _suite_section(version, files, "mini")
    lines += _extras_section(version, files)
    if notes.strip():
        lines += ["## What changed", "", notes.strip(), ""]
    text = "\n".join(lines)
    for bad in ("—", "–"):
        if bad in text:
            raise SystemExit("downloads.py: a dash character in the release body")
    return text


def self_test():
    v = "0.8.0"
    names = [
        "Gridline-0.8.0-macos-universal.dmg", "Gridline-0.8.0-windows-x64-setup.exe", "Gridline-0.8.0-windows-arm64-setup.exe",
        "Gridline-0.8.0-windows-x64.zip", "Gridline-0.8.0-linux-amd64.deb", "Gridline-0.8.0-linux-x86_64.rpm",
        "Gridline-0.8.0-linux-x86_64.AppImage", "Gridline-0.8.0-linux-aarch64.tar.gz", "Gridline-0.8.0-freebsd-x86_64.tar.gz",
        "Gridline-Suite-0.8.0-macos-universal.pkg", "Gridline-Suite-0.8.0-windows-setup.exe",
        "Gridline-Suite-0.8.0-linux-x86_64-setup.run", "Gridline-Suite-0.8.0-netbsd-x86_64-setup.run",
        "Gridline-Suite-0.8.0-macos-all.zip", "Gridline-Suite-0.8.0-bsd-all.zip",
        "Gridline-Suite-Plasma-0.8.0-macos.zip", "Gridline-Suite-AcidBass-0.8.0-linux.zip", "Gridline-Suite-Loupe-0.8.0-windows.zip",
        "Gridline-Suite-Mini-0.8.0-windows-setup.exe", "Gridline-Suite-Mini-0.8.0-linux-all.zip",
        "Gridline-Suite-Mini-Compressor-0.8.0-macos.zip",
        "Gridline-0.8.0-media-tools-linux-x86_64.zip", "Gridline-0.8.0-symbols-macos-universal.zip",
        "Gridline-0.8.0-source.tar.gz", "Gridline-0.8.0-SHA256SUMS",
    ]
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    got = {n: classify(n, v) for n in names}
    for n, e in got.items():
        check(e is not None, "placed: " + n)
    check(got["Gridline-0.8.0-linux-amd64.deb"]["format"] == "deb", "deb format")
    check(got["Gridline-0.8.0-linux-aarch64.tar.gz"]["format"] == "tar.gz", "tar.gz format")
    check(got["Gridline-0.8.0-freebsd-x86_64.tar.gz"]["family"] == "bsd", "freebsd is bsd")
    check(got["Gridline-0.8.0-windows-x64.zip"]["kind"] == "portable", "portable zip")
    check(got["Gridline-Suite-Plasma-0.8.0-macos.zip"] == {"name": "Gridline-Suite-Plasma-0.8.0-macos.zip", "kind": "single",
                                                          "product": "suite", "plugin": "Plasma", "family": "macos"}, "single plugin")
    check(got["Gridline-Suite-Mini-Compressor-0.8.0-macos.zip"]["product"] == "mini", "a Mini single")
    check(got["Gridline-Suite-Mini-0.8.0-linux-all.zip"]["kind"] == "all" and got["Gridline-Suite-Mini-0.8.0-linux-all.zip"]["product"] == "mini",
          "Mini all zip")
    check(got["Gridline-Suite-0.8.0-netbsd-x86_64-setup.run"]["family"] == "bsd", "BSD installer")
    check(got["Gridline-Suite-0.8.0-windows-setup.exe"]["product"] == "suite", "the suite setup is not the app's")
    check(classify("Gridline-0.7.9-macos-universal.dmg", v) is None, "another version is not placed")
    check(classify("notes.md", v) is None, "a stray file is not placed")
    data = {"version": v, "files": [dict(got[n], size=1) for n in names]}
    body = render_body(data, "- Something new.")
    sections = [l for l in body.splitlines() if l.startswith("## ")]
    check(sections == ["## Gridline", "## Gridline Suite", "## Gridline Suite Mini", "## Extras", "## What changed"],
          "four grouped sections in order, then the notes: %s" % sections)
    check("right-click" in body.split("## Gridline")[0], "the macOS note comes first")
    check(body.index("| Plasma |") < body.index("| Loupe |") < body.index("| AcidBass |"), "flagships first, then by name")
    check("Gridline-Suite-Mini-Compressor" not in body.split("## Gridline Suite Mini")[0], "Mini singles stay in their section")
    check("portable Windows zips" in body, "portable zips listed under More")
    for line in body.splitlines():
        if line.startswith("## "):
            nxt = body.splitlines()[body.splitlines().index(line) + 2]
            check(nxt and not nxt.startswith("|"), "a one-line note opens " + line)
    if fails:
        print("downloads.py self-test FAILED:\n  " + "\n  ".join(fails))
        return 1
    print("downloads.py self-test: ok (%d names)" % len(names))
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("list")
    a.add_argument("dir")
    a.add_argument("--version", required=True)
    a.add_argument("--sums")
    a.add_argument("--out", required=True)
    b = sub.add_parser("body")
    b.add_argument("list")
    b.add_argument("--notes")
    b.add_argument("--out", required=True)
    args = ap.parse_args(argv[1:])
    if args.cmd == "list":
        data = build_list(args.dir, args.version, args.sums)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1, sort_keys=True)
            f.write("\n")
    else:
        with open(args.list, encoding="utf-8") as f:
            data = json.load(f)
        notes = ""
        if args.notes:
            with open(args.notes, encoding="utf-8") as f:
                notes = f.read()
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(render_body(data, notes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
