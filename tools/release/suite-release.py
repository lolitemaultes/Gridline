#!/usr/bin/env python3
"""Gridline Suite and Suite Mini release files, as the release and packaging workflows make them.

    suite-release.py stage SRC OUT [--cli GRIDLINE] [--tar FILE]
    suite-release.py build --family F --version V --input [LABEL=]DIR [--input ...] --out DIR
                           [--license FILE] [--sign] [--iscc ISCC]
    suite-release.py check --family F --version V --dir DIR [--catalogues DIR] [--install]
    suite-release.py --self-test

One script for every system, so the release run and the packaging test run do the same thing.

stage   copies both suites' bundles out of a build (SRC/plugins) or an installed tree (SRC/lib/vst3,
        SRC/lib/clap) into OUT, without the sound sources. With --cli, the built `gridline` writes
        each suite's member catalogue next to them (catalogue-gridline.json, catalogue-mini.json).
        --tar packs OUT into one tar, which keeps the execute bits through a workflow artifact.
build   writes, for Gridline Suite and Suite Mini, the all-plugins zip (package-plugins.py) and the
        pick-your-plugins installers (suite-installer.py) of one system family:
          macos    LABEL empty: Gridline-Suite[-Mini]-V-macos-universal.pkg, -macos-all.zip
          windows  LABEL x64 / arm64: Gridline-Suite[-Mini]-V-windows-setup.exe (with --iscc; the
                   VST3 bundle carries both processors), -windows-all.zip
          linux    LABEL x86_64 / aarch64: Gridline-Suite[-Mini]-V-linux-LABEL-setup.run, -linux-all.zip
          bsd      LABEL freebsd-x86_64, ...: Gridline-Suite[-Mini]-V-LABEL-setup.run, -bsd-all.zip
        The catalogues come from the first input folder that has them.
check   the files of one family are all there and sound: every bundle in a zip lists every plugin
        of its suite and every plugin binary keeps its execute bit; a .pkg holds one choice per
        plugin and the system installer reads its choices; a .run lists the plugins. --install
        also installs the first .run (two plugins) into a scratch folder and removes it again.
        Prints each file's size; fails if one comes near GitHub's 2 GiB limit per release file.
Public text only: no task numbers, no other products.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SUITES = {"gridline": "Gridline Suite", "mini": "Gridline Suite Mini"}
ASSET = {"gridline": "Gridline-Suite", "mini": "Gridline-Suite-Mini"}
FAMILIES = ("macos", "windows", "linux", "bsd")
GITHUB_FILE_LIMIT = 2 * 1024 ** 3  # GitHub's limit per release file
SIZE_MARGIN = 0.9  # fail at 90 % of it


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _rmtree(p):
    if os.path.isdir(p) and not os.path.islink(p):
        shutil.rmtree(p)
    elif os.path.lexists(p):
        os.remove(p)


# ------------------------------------------------------------------ stage ----

def _bundle_dirs(src):
    """Where a build or an installed tree keeps the suite bundles."""
    out = []
    for rel in ("plugins", "lib/vst3", "lib/clap", ""):
        d = os.path.join(src, *rel.split("/")) if rel else src
        if os.path.isdir(d):
            out.append(d)
    return out


def stage(src, out, cli=None, tar=None):
    os.makedirs(out, exist_ok=True)
    found = []
    for sid, sname in SUITES.items():
        for d in _bundle_dirs(src):
            for suffix in (".vst3", ".clap", ".component", "-assets"):
                p = os.path.join(d, sname + suffix)
                dst = os.path.join(out, sname + suffix)
                if not os.path.lexists(p) or os.path.lexists(dst):
                    continue
                if os.path.isdir(p):
                    shutil.copytree(p, dst, symlinks=True)
                else:
                    shutil.copy2(p, dst)
                    if suffix == ".clap":  # a CLAP is one shared object; an installed tree may carry it as 644
                        os.chmod(dst, os.stat(dst).st_mode | 0o755)
                found.append(sname + suffix)
        for sub in (os.path.join(sname + ".vst3", "Contents", "Resources", "assets"),
                    os.path.join(sname + ".clap", "Contents", "Resources", "assets"),
                    os.path.join(sname + ".component", "Contents", "Resources", "assets"), sname + "-assets"):
            _rmtree(os.path.join(out, sub, "sounds", "src"))
        if not any(f.startswith(sname + ".") for f in found):
            raise SystemExit("no %s bundles under %s" % (sname, src))
        if cli:
            r = subprocess.run([cli, "suite-info", sid, "--json"], check=True, capture_output=True, text=True)
            cat = json.loads(r.stdout)
            if not cat:
                raise SystemExit("%s suite-info %s --json lists no plugins" % (cli, sid))
            with open(os.path.join(out, "catalogue-%s.json" % sid), "w", encoding="utf-8") as f:
                json.dump(cat, f, indent=1)
    for f in sorted(found):
        print("staged", f)
    if tar:
        pack(out, tar)
    return found


def pack(folder, tar):
    """One tar of a folder's contents: a workflow artifact keeps no execute bits, a tar does."""
    with tarfile.open(tar, "w") as t:
        for name in sorted(os.listdir(folder)):
            t.add(os.path.join(folder, name), arcname=name)


# ------------------------------------------------------------------ build ----

def _parse_inputs(values):
    out = []
    for v in values:
        label, sep, folder = v.partition("=")
        out.append((label, folder) if sep else ("", v))
    return out


def _catalogue(inputs, sid):
    for _, folder in inputs:
        p = os.path.join(folder, "catalogue-%s.json" % sid)
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                return json.load(f)
    raise SystemExit("no catalogue-%s.json in the input folders (stage it with --cli)" % sid)


def _license_text(path):
    if not path:
        return None
    with open(path, encoding="utf-8") as f:
        return f.read()


def _windows_setup(sid, version, catalogue, inputs, out, iscc, pp, si):
    """The Inno Setup script with its payload beside it (x64 and arm64 folders, the VST3 bundle
    carrying both processors, trimmed assets), compiled when iscc is given."""
    sname = SUITES[sid]
    work = os.path.join(out, "_setup-" + sid)
    _rmtree(work)
    os.makedirs(work)
    types = [m["type"] for m in catalogue]
    staged = {}
    for label, folder in inputs:
        d = os.path.join(work, label)
        os.makedirs(d)
        for suffix in (".vst3", ".clap", "-assets"):
            p = os.path.join(folder, sname + suffix)
            if os.path.isdir(p):
                shutil.copytree(p, os.path.join(d, sname + suffix), symlinks=True)
            elif os.path.isfile(p):
                shutil.copy2(p, os.path.join(d, sname + suffix))
        pp.trim_assets(os.path.join(d, sname + ".vst3", "Contents", "Resources", "assets"), types)
        pp.trim_assets(os.path.join(d, sname + "-assets"), types)
        staged[label] = d
    # The setup installs the VST3 bundle from the first processor's folder: give it every processor.
    labels = [l for l, _ in inputs]
    first = os.path.join(work, labels[0], sname + ".vst3")
    for label in labels[1:]:
        other = os.path.join(work, label, sname + ".vst3")
        if os.path.isdir(first) and os.path.isdir(other):
            pp.merge_vst3(first, other)
            shutil.rmtree(other)
    iss = si.build_iss(sid, version, catalogue, [(l, staged[l]) for l in labels], work)
    exe = "%s-%s-windows-setup.exe" % (ASSET[sid], version)
    if not iscc:
        print("no Inno Setup compiler given: %s stays a script in %s" % (iss, work))
        return None
    subprocess.run([iscc, "/Q", iss], cwd=work, check=True)
    shutil.move(os.path.join(work, exe), os.path.join(out, exe))
    shutil.rmtree(work)
    return exe


def build(family, version, inputs, out, license_path=None, sign=False, iscc=None):
    pp = _load("package_plugins", "package-plugins.py")
    si = _load("suite_installer", "suite-installer.py")
    lic = _license_text(license_path)
    os.makedirs(out, exist_ok=True)
    written = []
    for sid in SUITES:
        cat = _catalogue(inputs, sid)
        # The all-plugins zip.
        written += pp.package(sid, family, version, cat, inputs, out, license_path, sign=sign and family == "macos")
        # The installers.
        if family == "macos":
            written.append(si.build_pkg(sid, version, cat, inputs[0][1], out, lic))
        elif family == "windows":
            exe = _windows_setup(sid, version, cat, inputs, out, iscc, pp, si)
            if exe:
                written.append(exe)
        else:
            for label, folder in inputs:
                written.append(si.build_run(sid, version, ("linux-" + label) if family == "linux" else label, cat, folder, out, lic))
    for n in written:
        print(n)
    return written


# ------------------------------------------------------------------ check ----

_BIN_DIR = re.compile(r"/Contents/(MacOS|[a-z0-9_]+-(win|linux|freebsd|openbsd|netbsd))/[^/]+$")


def _zip_problems(path, types_by_suite):
    """What is wrong with an all-plugins zip (an empty list: nothing)."""
    bad = []
    sid = "mini" if os.path.basename(path).startswith("Gridline-Suite-Mini-") else "gridline"
    want = set(types_by_suite[sid]) if types_by_suite.get(sid) else None
    with zipfile.ZipFile(path) as z:
        infos = z.infolist()
        lists = [i for i in infos if i.filename.endswith("/gridline-members.txt")]
        if not lists:
            bad.append("no member lists")
        for i in lists:
            got = [l for l in z.read(i).decode("utf-8").splitlines() if l and not l.startswith("#")]
            if not got or got[0] != "suite " + sid:
                bad.append("%s names suite %r" % (i.filename, got[:1]))
            if want is not None and set(got[1:]) != want:
                bad.append("%s lists %d plugins, the suite has %d" % (i.filename, len(set(got[1:])), len(want)))
        binaries = [i for i in infos if not i.filename.endswith("/") and
                    (_BIN_DIR.search(i.filename) or i.filename.endswith(".clap"))]
        if not binaries:
            bad.append("no plugin binaries")
        for i in binaries:
            mode = (i.external_attr >> 16) & 0o777
            if mode & 0o111 != 0o111:
                bad.append("%s has mode %o, not executable" % (i.filename, mode))
        for top in ("README.txt", "PLUGINS.txt"):
            if not any(i.filename.endswith("/" + top) and i.filename.count("/") == 1 for i in infos):
                bad.append("no " + top)
    return bad, len(binaries), len(lists)


def check(family, version, folder, catalogues=None, install=False):
    fails = []
    types = {}
    for sid in SUITES:
        p = os.path.join(catalogues, "catalogue-%s.json" % sid) if catalogues else None
        if p and os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                types[sid] = [m["type"] for m in json.load(f)]
    dl = _load("downloads", "downloads.py")
    names = sorted(n for n in os.listdir(folder) if n.startswith("Gridline-Suite-") and os.path.isfile(os.path.join(folder, n)))
    for sid in SUITES:
        expect = ["%s-%s-%s-all.zip" % (ASSET[sid], version, family)]
        if family == "macos":
            expect.append("%s-%s-macos-universal.pkg" % (ASSET[sid], version))
        elif family == "windows":
            expect.append("%s-%s-windows-setup.exe" % (ASSET[sid], version))
        for e in expect:
            if e not in names:
                fails.append("missing " + e)
        runs = [n for n in names if n.startswith(ASSET[sid] + "-" + version + "-") and n.endswith("-setup.run")]
        if family in ("linux", "bsd") and not runs:
            fails.append("no %s .run installers" % SUITES[sid])
    print("%-62s %12s" % ("file", "bytes"))
    total = 0
    for n in names:
        p = os.path.join(folder, n)
        size = os.path.getsize(p)
        total += size
        print("%-62s %12d  (%.1f MB)" % (n, size, size / 1e6))
        e = dl.classify(n, version)
        if e is None or e.get("product") not in ("suite", "mini"):
            fails.append("the release page cannot place " + n)
        if size > GITHUB_FILE_LIMIT * SIZE_MARGIN:
            fails.append("%s is %.0f MB, near GitHub's 2 GiB limit per release file" % (n, size / 1e6))
        if n.endswith("-all.zip"):
            bad, nbin, nlists = _zip_problems(p, types)
            print("    %d plugin binaries, %d member lists%s" % (nbin, nlists, "" if not bad else ": " + "; ".join(bad[:5])))
            fails += [n + ": " + b for b in bad]
        elif n.endswith(".pkg"):
            fails += [n + ": " + b for b in _pkg_problems(p, n, types)]
        elif n.endswith(".run"):
            r = subprocess.run(["sh", p, "--list"], stdin=subprocess.DEVNULL, capture_output=True, text=True,
                               env=dict(os.environ, TMPDIR=tempfile.gettempdir()))
            sid = "mini" if n.startswith("Gridline-Suite-Mini-") else "gridline"
            m = re.search(r"(\d+) plugins", r.stdout)
            if r.returncode != 0 or not m:
                fails.append("%s --list: %s" % (n, (r.stdout + r.stderr).strip()[-200:]))
            elif sid in types and int(m.group(1)) != len(types[sid]):
                fails.append("%s lists %s plugins, the suite has %d" % (n, m.group(1), len(types[sid])))
    print("%-62s %12d  (%.1f MB)" % ("total", total, total / 1e6))
    if install:
        runs = [n for n in names if n.endswith("-setup.run")]
        if runs:
            sid = "mini" if runs[0].startswith("Gridline-Suite-Mini-") else "gridline"
            if sid in types:
                fails += _run_install(os.path.join(folder, runs[0]), [types[sid][0], types[sid][-1]])
            else:
                fails.append("--install needs --catalogues")
        for n in names:
            if n.endswith("-windows-setup.exe") and os.name == "nt":
                sid = "mini" if n.startswith("Gridline-Suite-Mini-") else "gridline"
                fails += _setup_install(os.path.join(folder, n), SUITES[sid], types.get(sid))
    for f in fails:
        print("FAIL", f)
    print("suite release files (%s): %s" % (family, "ok" if not fails else "%d problems" % len(fails)))
    return 1 if fails else 0


def _pkg_problems(path, name, types):
    bad = []
    if not shutil.which("pkgutil"):
        return bad
    sid = "mini" if name.startswith("Gridline-Suite-Mini-") else "gridline"
    with tempfile.TemporaryDirectory() as tmp:
        ex = os.path.join(tmp, "x")
        subprocess.run(["pkgutil", "--expand", path, ex], check=True)
        parts = os.listdir(ex)
        choices = sum(1 for p in parts if p.startswith("p.") and p.endswith(".pkg"))
        if sid in types and choices != len(types[sid]):
            bad.append("%d plugin choices, the suite has %d" % (choices, len(types[sid])))
        for f in ("vst3", "clap", "component"):
            if "fmt-%s.pkg" % f not in parts:
                bad.append("no %s package" % f)
    # The system installer reads the package and lists its choices (nothing is installed).
    r = subprocess.run(["installer", "-showChoicesXML", "-pkg", path, "-target", "CurrentUserHomeDirectory"],
                       capture_output=True, text=True)
    if r.returncode != 0 or "<plist" not in r.stdout:
        bad.append("installer -showChoicesXML: " + (r.stdout + r.stderr).strip()[-200:])
    else:
        n = r.stdout.count("<string>p.gl_")
        print("    the system installer reads %d plugin choices" % n)
        if sid in types and n < len(types[sid]):
            bad.append("the system installer shows %d plugin choices, the suite has %d" % (n, len(types[sid])))
    return bad


def _run_install(run, picks):
    """Installs two plugins from a .run into a scratch folder (scratch home too), then uninstalls."""
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        home = os.path.join(tmp, "home")
        dest = os.path.join(tmp, "dest")
        os.makedirs(home)
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
                   XDG_DATA_HOME=os.path.join(home, ".local", "share"), TMPDIR=tmp)
        env.pop("GRIDLINE_CONFIG_HOME", None)
        r = subprocess.run(["sh", run, "--dest", dest, "--yes", "--plugins", ",".join(picks), "--no-update-check"],
                           env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True)
        print("    install %s: %s" % (",".join(picks), "ok" if r.returncode == 0 else "failed"))
        if r.returncode != 0:
            return ["install: " + (r.stdout + r.stderr).strip()[-300:]]
        lists = []
        for d, _, files in os.walk(dest):
            lists += [os.path.join(d, f) for f in files if f == "gridline-members.txt"]
        if not lists:
            bad.append("install wrote no member list")
        for p in lists:
            with open(p, encoding="utf-8") as f:
                got = [l for l in f.read().splitlines() if l and not l.startswith("#")][1:]
            if sorted(got) != sorted(picks):
                bad.append("%s lists %r, picked %r" % (p, got, picks))
        clap = [os.path.join(dest, "clap", n) for n in os.listdir(os.path.join(dest, "clap"))] if os.path.isdir(os.path.join(dest, "clap")) else []
        for p in clap:
            if p.endswith(".clap") and os.path.isfile(p) and not os.access(p, os.X_OK):
                bad.append(p + " is not executable")
        unins = [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(dest, "share")) for f in fs if f == "uninstall"]
        if not unins:
            bad.append("no uninstall script")
        else:
            subprocess.run(["sh", unins[0]], env=env, capture_output=True, text=True)
            left = [n for n in ("vst3", "clap") if os.path.isdir(os.path.join(dest, n)) and os.listdir(os.path.join(dest, n))]
            if left:
                bad.append("uninstall left files in " + ", ".join(left))
    return bad


def _setup_install(exe, sname, types):
    """Windows: a silent install for this user only (every plugin, every format), the member lists
    it wrote, then its own uninstaller. Nothing goes into the system's folders."""
    bad = []
    log = os.path.join(tempfile.gettempdir(), os.path.basename(exe) + ".log")
    r = subprocess.run([exe, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/LOG=" + log])
    print("    silent install for this user: exit %d" % r.returncode)
    if r.returncode != 0:
        return ["%s: silent install exit %d (log %s)" % (os.path.basename(exe), r.returncode, log)]
    common = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Common")
    lists = [os.path.join(common, "VST3", sname + ".vst3", "Contents", "Resources", "gridline-members.txt"),
             os.path.join(common, "CLAP", sname + "-assets", "gridline-members.txt")]
    for p in lists:
        if not os.path.isfile(p):
            bad.append("no member list at " + p)
            continue
        with open(p, encoding="utf-8") as f:
            got = [l for l in f.read().splitlines() if l and not l.startswith("#")][1:]
        if types is not None and set(got) != set(types):
            bad.append("%s lists %d plugins, the suite has %d" % (p, len(set(got)), len(types)))
    if not os.path.isfile(os.path.join(common, "CLAP", sname + ".clap")):
        bad.append("no CLAP installed in " + common)
    unins = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Gridline", sname, "unins000.exe")
    if not os.path.isfile(unins):
        bad.append("no uninstaller at " + unins)
    else:
        subprocess.run([unins, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"])
        import time
        for _ in range(60):  # the uninstaller finishes in a copy of itself
            if not os.path.exists(os.path.join(common, "CLAP", sname + ".clap")):
                break
            time.sleep(1)
        if os.path.exists(os.path.join(common, "CLAP", sname + ".clap")):
            bad.append("the uninstaller left " + sname + ".clap")
    return bad


# -------------------------------------------------------------- self test ----

def self_test():
    """Stage, build and check a whole Linux family from stand-in bundles (no compiler, no network)."""
    fails = []
    pp = _load("package_plugins", "package-plugins.py")
    cats = {"gridline": [{"type": "gl.plasma", "name": "Plasma", "kind": "instrument", "category": "Synth", "au": "aumu/plas/GrLn"},
                         {"type": "gl.reverb", "name": "Reverb", "kind": "effect", "category": "Reverb", "au": "aufx/revb/GrLn"}],
            "mini": [{"type": "gl.minitrim", "name": "Trim", "kind": "effect", "category": "Utility", "au": "aufx/mtrm/GrLn"}]}
    with tempfile.TemporaryDirectory() as tmp:
        ins = []
        for arch in ("x86_64", "aarch64"):
            build_dir = os.path.join(tmp, "build-" + arch)
            for sname in SUITES.values():
                for fmt in ("vst3", "clap"):
                    pp._fake_bundle(os.path.join(build_dir, "plugins"), sname, fmt, "linux", arch + "-linux")
            _rmtree(os.path.join(build_dir, "plugins", "Gridline Suite-assets", "sounds"))
            os.makedirs(os.path.join(build_dir, "plugins", "Gridline Suite-assets", "sounds", "src"))
            out = os.path.join(tmp, "stage-" + arch)
            stage(build_dir, out)
            if os.path.isdir(os.path.join(out, "Gridline Suite-assets", "sounds", "src")):
                fails.append("stage leaves the sound sources in")
            for sid, cat in cats.items():
                with open(os.path.join(out, "catalogue-%s.json" % sid), "w") as f:
                    json.dump(cat, f)
            pack(out, os.path.join(tmp, arch + ".tar"))
            unpacked = os.path.join(tmp, "unpacked-" + arch)
            with tarfile.open(os.path.join(tmp, arch + ".tar")) as t:
                t.extractall(unpacked)
            if not os.access(os.path.join(unpacked, "Gridline Suite.clap"), os.X_OK):
                fails.append("the stage tar loses the execute bit")
            ins.append((arch, unpacked))
        dist = os.path.join(tmp, "dist")
        names = build("linux", "0.8.0", ins, dist)
        want = {"Gridline-Suite-0.8.0-linux-all.zip", "Gridline-Suite-Mini-0.8.0-linux-all.zip",
                "Gridline-Suite-0.8.0-linux-x86_64-setup.run", "Gridline-Suite-0.8.0-linux-aarch64-setup.run",
                "Gridline-Suite-Mini-0.8.0-linux-x86_64-setup.run", "Gridline-Suite-Mini-0.8.0-linux-aarch64-setup.run"}
        if set(names) != want:
            fails.append("linux files: %r" % sorted(names))
        if check("linux", "0.8.0", dist, ins[0][1], install=True) != 0:
            fails.append("the check of good files fails")
        # A zip that lost its execute bits, or a member, is caught.
        bad = os.path.join(tmp, "bad")
        os.makedirs(bad)
        src = os.path.join(dist, "Gridline-Suite-0.8.0-linux-all.zip")
        with zipfile.ZipFile(src) as zi, zipfile.ZipFile(os.path.join(bad, os.path.basename(src)), "w") as zo:
            for i in zi.infolist():
                data = zi.read(i)
                if i.filename.endswith(".clap"):
                    i.external_attr = (0o100644 << 16)
                if i.filename.endswith("gridline-members.txt"):
                    data = data.replace(b"gl.reverb\n", b"")
                zo.writestr(i, data)
        problems, _, _ = _zip_problems(os.path.join(bad, os.path.basename(src)), {"gridline": ["gl.plasma", "gl.reverb"]})
        if not any("not executable" in p for p in problems):
            fails.append("a lost execute bit is not caught")
        if not any("lists 1 plugins" in p for p in problems):
            fails.append("a missing member is not caught")
        # Windows without the compiler: the script and its payload, the VST3 bundle with both processors.
        wins = []
        for label, arch in (("x64", "x86_64-win"), ("arm64", "arm64-win")):
            d = os.path.join(tmp, "win-" + label)
            for sname in SUITES.values():
                for fmt in ("vst3", "clap"):
                    pp._fake_bundle(d, sname, fmt, "windows", arch)
            for sid, cat in cats.items():
                with open(os.path.join(d, "catalogue-%s.json" % sid), "w") as f:
                    json.dump(cat, f)
            wins.append((label, d))
        wdist = os.path.join(tmp, "wdist")
        wnames = build("windows", "0.8.0", wins, wdist)
        if sorted(wnames) != ["Gridline-Suite-0.8.0-windows-all.zip", "Gridline-Suite-Mini-0.8.0-windows-all.zip"]:
            fails.append("windows files without the compiler: %r" % wnames)
        work = os.path.join(wdist, "_setup-gridline")
        if not os.path.isfile(os.path.join(work, "Gridline-Suite-0.8.0-windows.iss")):
            fails.append("no Inno script")
        v = os.path.join(work, "x64", "Gridline Suite.vst3", "Contents")
        if not (os.path.isdir(os.path.join(v, "x86_64-win")) and os.path.isdir(os.path.join(v, "arm64-win"))):
            fails.append("the setup's VST3 bundle lacks a processor")
        if os.path.isdir(os.path.join(work, "x64", "Gridline Suite.vst3", "Contents", "Resources", "assets", "sounds")):
            fails.append("the setup carries untrimmed assets")
        if not os.path.isfile(os.path.join(work, "arm64", "Gridline Suite.clap")):
            fails.append("the setup lacks the ARM CLAP")
    for f in fails:
        print("FAIL", f)
    print("suite-release self-test: %s" % ("ok" if not fails else "%d failed" % len(fails)))
    return 1 if fails else 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("src")
    s.add_argument("out")
    s.add_argument("--cli")
    s.add_argument("--tar")
    b = sub.add_parser("build")
    b.add_argument("--family", choices=FAMILIES, required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--input", action="append", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--license")
    b.add_argument("--sign", action="store_true")
    b.add_argument("--iscc")
    c = sub.add_parser("check")
    c.add_argument("--family", choices=FAMILIES, required=True)
    c.add_argument("--version", required=True)
    c.add_argument("--dir", required=True)
    c.add_argument("--catalogues")
    c.add_argument("--install", action="store_true")
    a = ap.parse_args(argv[1:])
    if a.cmd == "stage":
        stage(a.src, a.out, a.cli, a.tar)
        return 0
    if a.cmd == "build":
        lic = a.license if a.license and os.path.isfile(a.license) else None
        build(a.family, a.version, _parse_inputs(a.input), a.out, lic, a.sign, a.iscc)
        return 0
    return check(a.family, a.version, a.dir, a.catalogues, a.install)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
