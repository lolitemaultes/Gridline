#!/usr/bin/env python3
"""Gridline Suite and Suite Mini plugin downloads: the all-plugins zip.

    package-plugins.py --suite gridline|mini --family macos|windows|linux|bsd --version V
                       --catalogue members.json --input [LABEL=]DIR [--input ...]
                       --out DIR [--license LICENSE.txt] [--sign] [--singles | --singles-only]
    package-plugins.py --self-test

Every plugin of a suite is the same engine binary; the member list inside a bundle
("gridline-members.txt") says which plugins it shows to hosts. From the whole-suite bundles the
build made (one per format, "Gridline Suite.vst3", ".clap", ".component"), this writes the
all-plugins zip, Gridline-Suite[-Mini]-<V>-<family>-all.zip: the whole-suite bundles, a PLUGINS.txt
list, README.txt and LICENSE.txt. People who want some of the plugins use the installers
(suite-installer.py), which tick plugins and formats.

Single-plugin zips are not part of a release: each carries the whole engine and is as large as the
whole suite. Lean per-plugin binaries are planned; until then --singles (or --singles-only) still
writes them, for tests only: one zip per plugin, Gridline-Suite[-Mini]-<Plugin>-<V>-<family>.zip,
whose bundles are copies of the suite's renamed to the plugin (Plasma.vst3, Plasma.clap,
Plasma.component) with a member list naming that plugin alone. The release page does not place them.

--catalogue is `gridline suite-info <suite> --json`. Each --input is a folder holding the suite's
bundles for one processor; LABEL names the processor in the archive (CLAP/x64/...) and for BSD the
system (freebsd-x86_64). VST3 bundles of several processors merge into one bundle, as the VST3
layout intends (Contents/x86_64-linux, Contents/aarch64-linux). On macOS the Audio Unit's
Info.plist keeps only the plugin's component, each bundle gets its own identifier, and --sign
re-signs the changed bundles ad hoc (codesign --sign -).

Assets are trimmed per bundle: a plugin reads only the fonts, the translations and, for a few
plugins, one more folder (ASSETS_BY_TYPE); the app's sounds, templates, demo, icons and scripts stay
out. On macOS the Audio Unit's view factory class gets a name of its own per single-plugin bundle
(the binary's "Factory_T0000000000000000" tag is rewritten), so two single plugins loaded in one host
never share an Objective-C class.

Zips are reproducible: sorted entries, a fixed time (SOURCE_DATE_EPOCH, else 2000-01-01) and
stored Unix modes, so Linux and BSD keep execute bits and macOS bundles stay whole. Public text
only: no task numbers, no other products.
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

SUITES = {
    "gridline": {"name": "Gridline Suite", "asset": "Gridline-Suite", "bundle_id": "org.gridline.GridlineSuite"},
    "mini": {"name": "Gridline Suite Mini", "asset": "Gridline-Suite-Mini", "bundle_id": "org.gridline.GridlineSuiteMini"},
}
FAMILIES = ("macos", "windows", "linux", "bsd")
MEMBER_LIST = "gridline-members.txt"

# What a plugin reads from its assets folder at run time: every plugin the fonts and the
# translations (.po / .json; the .pot template is for translators), and these plugins one more
# folder each. Everything else in the app's assets (interface sounds, templates, the demo project,
# icons and wordmarks, which the plugins carry compiled in) stays out of plugin bundles.
ASSETS_ALWAYS = ("fonts", "i18n")
ASSETS_BY_TYPE = {
    "gl.edison": ("scripts/audio",),  # Loupe's Tools > Scripts lists the shipped audio scripts
}
AU_FACTORY_TAG = re.compile(rb"Factory_T0{16}")

MAC_NOTE = ("These plugins are not signed by Apple. If a host says a plugin cannot be opened, open System\n"
            "Settings > Privacy & Security and click Open Anyway, or run once in Terminal:\n"
            "  xattr -dr com.apple.quarantine <the plugin file>\n")

WHERE = {
    "macos": ("Copy each file to its folder (create the folder if it is missing):\n"
              "  VST3/...       ->  ~/Library/Audio/Plug-Ins/VST3/\n"
              "  CLAP/...       ->  ~/Library/Audio/Plug-Ins/CLAP/\n"
              "  Components/... ->  ~/Library/Audio/Plug-Ins/Components/   (Audio Unit)\n"),
    "windows": ("Copy each folder's contents to (create the folder if it is missing):\n"
                "  VST3/...         ->  C:\\Program Files\\Common Files\\VST3\\\n"
                "  CLAP/x64/...     ->  C:\\Program Files\\Common Files\\CLAP\\   (Intel and AMD processors)\n"
                "  CLAP/arm64/...   ->  C:\\Program Files\\Common Files\\CLAP\\   (ARM processors)\n"
                "Keep each .clap file next to its -assets folder.\n"),
    "linux": ("Copy each folder's contents to (create the folder if it is missing):\n"
              "  VST3/...             ->  ~/.vst3/\n"
              "  CLAP/<processor>/... ->  ~/.clap/   (x86_64 for Intel and AMD, aarch64 for ARM)\n"
              "Keep each .clap file next to its -assets folder.\n"),
    "bsd": ("Pick the folder for your system (freebsd, openbsd or netbsd), then copy:\n"
            "  VST3/...  ->  ~/.vst3/\n"
            "  CLAP/...  ->  ~/.clap/\n"
            "Keep each .clap file next to its -assets folder.\n"),
}


def slug(name):
    """The plugin's name in file names: words kept, capitalised, joined ("Hi-hat" -> "HiHat")."""
    words = re.findall(r"[A-Za-z0-9]+", name)
    return "".join(w[:1].upper() + w[1:] for w in words)


def member_list(suite, types):
    return "# Gridline plugin bundle: the plugins it shows to hosts\nsuite %s\n%s" % (suite, "".join(t + "\n" for t in types))


def single_name(suite, plugin_slug, version, family):
    return "%s-%s-%s-%s.zip" % (SUITES[suite]["asset"], plugin_slug, version, family)


def all_name(suite, version, family):
    return "%s-%s-%s-all.zip" % (SUITES[suite]["asset"], version, family)


# ---------------------------------------------------------------- bundles ----

def _formats(folder, suite_name):
    """The suite's bundles in one input folder: {'vst3': path, 'clap': path, 'component': path}."""
    out = {}
    for ext in ("vst3", "clap", "component"):
        p = os.path.join(folder, suite_name + "." + ext)
        if os.path.exists(p):
            out[ext] = p
    return out


def _copy(src, dst):
    if os.path.isdir(src):
        shutil.copytree(src, dst, symlinks=True)
    else:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def _rename_binaries(bundle, old_stem, new_stem):
    """VST3 on Windows and Linux: Contents/<arch>/<bundle name>.vst3 / .so must match the bundle name."""
    contents = os.path.join(bundle, "Contents")
    if not os.path.isdir(contents):
        return
    for arch in sorted(os.listdir(contents)):
        d = os.path.join(contents, arch)
        if not os.path.isdir(d) or arch in ("Resources", "MacOS"):
            continue
        for f in sorted(os.listdir(d)):
            stem, ext = os.path.splitext(f)
            if stem == old_stem:
                os.rename(os.path.join(d, f), os.path.join(d, new_stem + ext))


def _edit_plist(bundle, ident, au_codes=None):
    """macOS: a bundle identifier of its own and, for the Audio Unit, only the listed components."""
    p = os.path.join(bundle, "Contents", "Info.plist")
    if not os.path.isfile(p):
        return
    with open(p, "rb") as f:
        pl = plistlib.load(f)
    pl["CFBundleIdentifier"] = ident
    if au_codes is not None and isinstance(pl.get("AudioComponents"), list):
        pl["AudioComponents"] = [c for c in pl["AudioComponents"] if (c.get("type"), c.get("subtype")) in au_codes]
    with open(p, "wb") as f:
        plistlib.dump(pl, f, sort_keys=False)


def trim_assets(root, types):
    """Keeps in the assets folder `root` only what the plugins `types` read (top-level files stay)."""
    if not os.path.isdir(root):
        return
    keep = set(ASSETS_ALWAYS)
    for t in types:
        keep.update(ASSETS_BY_TYPE.get(t, ()))

    def prune(folder, rel):
        for name in sorted(os.listdir(folder)):
            p = os.path.join(folder, name)
            r = rel + name
            if os.path.isdir(p) and not os.path.islink(p):
                if r in keep:
                    for d, _, files in os.walk(p):  # kept whole, but the translators' template
                        for f in files:
                            if f.endswith(".pot"):
                                os.remove(os.path.join(d, f))
                elif any(k.startswith(r + "/") for k in keep):
                    prune(p, r + "/")  # leads to a kept folder
                else:
                    shutil.rmtree(p)
            elif rel:  # files beside a kept folder's parents go; the root's own files stay
                os.remove(p)

    prune(root, "")


def _bundle_assets(bundle):
    """The assets folder of a bundle folder or of a CLAP file (its -assets folder beside it)."""
    if os.path.isdir(bundle):
        return os.path.join(bundle, "Contents", "Resources", "assets")
    return os.path.join(os.path.dirname(bundle), os.path.splitext(os.path.basename(bundle))[0] + "-assets")


def tag_au_factory(bundle, ident):
    """macOS: the Audio Unit's view factory class gets this bundle's own name (see AuEntry.mm).
    Returns how many places were rewritten (0 when the binary has no tag slot)."""
    p = os.path.join(bundle, "Contents", "Info.plist")
    if not os.path.isfile(p):
        return 0
    with open(p, "rb") as f:
        exe = plistlib.load(f).get("CFBundleExecutable", "")
    binary = os.path.join(bundle, "Contents", "MacOS", exe)
    if not exe or not os.path.isfile(binary):
        return 0
    tag = b"Factory_T" + hashlib.sha256(ident.encode("utf-8")).hexdigest()[:16].encode("ascii")
    with open(binary, "rb") as f:
        data = f.read()
    new, n = AU_FACTORY_TAG.subn(tag, data)
    if n:
        mode = os.stat(binary).st_mode
        with open(binary, "wb") as f:
            f.write(new)
        os.chmod(binary, stat.S_IMODE(mode))
    return n


def _sign(bundle):
    subprocess.run(["codesign", "--force", "--sign", "-", bundle], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_bundle(src, dst, suite, types, family, plugin_slug=None, au_codes=None, sign=False):
    """Copies a suite bundle to dst and writes its member list (plugin_slug: a single-plugin bundle)."""
    _copy(src, dst)
    ext = os.path.splitext(dst)[1]
    old_stem = os.path.splitext(os.path.basename(src))[0]
    new_stem = os.path.splitext(os.path.basename(dst))[0]
    text = member_list(suite, types)
    if os.path.isdir(dst):  # a bundle folder (macOS every format; VST3 everywhere)
        _write(os.path.join(dst, "Contents", "Resources", MEMBER_LIST), text)
        if ext == ".vst3" and family != "macos":
            _rename_binaries(dst, old_stem, new_stem)
    else:  # a Windows / Linux / BSD CLAP file: its member list goes in the -assets folder beside it
        assets_src = os.path.join(os.path.dirname(src), old_stem + "-assets")
        assets_dst = os.path.join(os.path.dirname(dst), new_stem + "-assets")
        if os.path.isdir(assets_src) and not os.path.isdir(assets_dst):
            shutil.copytree(assets_src, assets_dst, symlinks=True)
        _write(os.path.join(assets_dst, MEMBER_LIST), text)
    trim_assets(_bundle_assets(dst), types)
    if family == "macos":
        ident = SUITES[suite]["bundle_id"] + ("." + plugin_slug if plugin_slug else "") + ext
        _edit_plist(dst, ident, au_codes if ext == ".component" else None)
        if ext == ".component" and plugin_slug:
            tag_au_factory(dst, ident)
        if sign:
            _sign(dst)


def merge_vst3(target, src):
    """Adds src's per-processor folders (Contents/<arch>) to the VST3 bundle at target."""
    contents = os.path.join(src, "Contents")
    for arch in sorted(os.listdir(contents)):
        s = os.path.join(contents, arch)
        t = os.path.join(target, "Contents", arch)
        if os.path.isdir(s) and not os.path.exists(t):
            shutil.copytree(s, t, symlinks=True)
    _rename_binaries(target, os.path.splitext(os.path.basename(src))[0], os.path.splitext(os.path.basename(target))[0])


# ------------------------------------------------------------- archives ----

def _fixed_time():
    try:
        t = int(os.environ.get("SOURCE_DATE_EPOCH", ""))
    except ValueError:
        t = 946684800
    import time
    g = time.gmtime(max(t, 315532800))
    return (g.tm_year, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec)


# Where the file system keeps no Unix execute bit (Windows: os.stat reports 0o666 for any file that
# is not .exe/.bat/.cmd/.com), the zip's mode is inferred from what the file is instead: plugin
# binaries by name or by their place in a bundle, other programs by their magic bytes.
_EXEC_EXTS = (".clap", ".so", ".dll", ".dylib", ".exe", ".vst3")
_EXEC_MAGIC = (b"\x7fELF", b"MZ", b"#!", b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe")


def _executable(p, st):
    if os.name != "nt":
        return bool(st.st_mode & 0o111)
    if p.lower().endswith(_EXEC_EXTS):
        return True
    parent = os.path.basename(os.path.dirname(p))
    if parent == "MacOS" or parent.endswith(("-win", "-linux", "-freebsd", "-netbsd", "-openbsd")):
        return True  # a VST3 / macOS bundle's binary folder
    try:
        with open(p, "rb") as f:
            head = f.read(4)
    except OSError:
        return False
    return head.startswith(_EXEC_MAGIC)


def write_zip(root, out_path):
    """Zips the folder `root` (its own name is the top entry), sorted, fixed times, Unix modes kept."""
    when = _fixed_time()
    base = os.path.dirname(root)
    entries = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for name in dirs:
            p = os.path.join(d, name)
            if os.path.islink(p):
                entries.append(p)
        entries.append(d)
        for name in sorted(files):
            entries.append(os.path.join(d, name))
    entries = sorted(set(entries), key=lambda p: os.path.relpath(p, base).replace(os.sep, "/"))
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in entries:
            rel = os.path.relpath(p, base).replace(os.sep, "/")
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                zi = zipfile.ZipInfo(rel, when)
                zi.create_system = 3
                zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                z.writestr(zi, os.readlink(p))
            elif stat.S_ISDIR(st.st_mode):
                zi = zipfile.ZipInfo(rel + "/", when)
                zi.create_system = 3
                zi.external_attr = ((stat.S_IFDIR | 0o755) << 16) | 0x10
                z.writestr(zi, b"")
            else:
                zi = zipfile.ZipInfo(rel, when)
                zi.create_system = 3
                mode = 0o755 if _executable(p, st) else 0o644
                zi.external_attr = (stat.S_IFREG | mode) << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(p, "rb") as f:
                    z.writestr(zi, f.read())


def readme(suite, family, version, what):
    text = "%s %s, %s\n\n%s\n\n%s" % (SUITES[suite]["name"], version, what, WHERE[family],
                                       "Then rescan plugins in your host. The plugins look for updates once a day\n"
                                       "(you can turn that off in any plugin's menu) and never install anything themselves.\n")
    if family == "macos":
        text += "\n" + MAC_NOTE
    return text


# ----------------------------------------------------------------- main ----

def _place(family, label, fmt, name):
    """Where a bundle goes inside the archive's top folder."""
    vendor = "Gridline Suite"
    if family == "macos":
        return {"vst3": ["VST3", vendor, name], "clap": ["CLAP", vendor, name], "component": ["Components", name]}[fmt]
    if family == "bsd":
        return [label or "bsd", fmt.upper(), vendor, name]
    if fmt == "vst3":
        return ["VST3", vendor, name]
    return ["CLAP"] + ([label] if label else []) + [vendor, name]


def package(suite, family, version, catalogue, inputs, out_dir, license_path=None, sign=False, only=None, singles=False,
            singles_only=False):
    """Writes the all-plugins zip (and, for tests only, single-plugin zips); returns the file names written."""
    singles = singles or singles_only
    sname = SUITES[suite]["name"]
    members = catalogue
    slugs = {}
    for m in members:
        s = slug(m["name"])
        if not s or s in slugs.values():
            raise SystemExit("plugin name %r gives an empty or repeated file name %r" % (m["name"], s))
        slugs[m["type"]] = s
    groups = []  # (label, {fmt: path}); BSD: one group per system
    for label, folder in inputs:
        f = _formats(folder, sname)
        if not f:
            raise SystemExit("no %s bundles in %s" % (sname, folder))
        groups.append((label, f))
    os.makedirs(out_dir, exist_ok=True)
    written = []
    license_text = None
    if license_path:
        with open(license_path, encoding="utf-8") as f:
            license_text = f.read()

    def fill(top, types, display, plugin_slug=None, au_codes=None):
        merged = {}  # VST3 target per BSD system ("" elsewhere)
        for label, fmts in groups:
            for fmt in sorted(fmts):
                ext = "." + fmt
                dst = os.path.join(top, *_place(family, label, fmt, display + ext))
                key = (label if family == "bsd" else "", fmt)
                if fmt == "vst3" and family in ("windows", "linux") and key in merged:
                    merge_vst3(merged[key], fmts[fmt])
                    continue
                if os.path.exists(dst):
                    continue
                make_bundle(fmts[fmt], dst, suite, types, family, plugin_slug, au_codes, sign)
                if fmt == "vst3":
                    merged[key] = dst
        if license_text is not None:
            _write(os.path.join(top, "LICENSE.txt"), license_text)

    with tempfile.TemporaryDirectory() as tmp:
        for m in members:
            if not singles or (only and m["type"] not in only):
                continue
            s = slugs[m["type"]]
            top = os.path.join(tmp, "single", s, m["name"])
            au = m.get("au", "").split("/")
            fill(top, [m["type"]], m["name"], s, {(au[0], au[1])} if len(au) >= 2 else set())
            _write(os.path.join(top, "README.txt"), readme(suite, family, version, m["name"] + " on its own"))
            name = single_name(suite, s, version, family)
            write_zip(top, os.path.join(out_dir, name))
            written.append(name)
        if singles_only:
            return written
        top = os.path.join(tmp, "all", sname)
        fill(top, [m["type"] for m in members], sname)
        _write(os.path.join(top, "README.txt"), readme(suite, family, version, "every plugin"))
        lines = ["%s %s: %d plugins" % (sname, version, len(members)), ""]
        for kind, title in (("instrument", "Instruments"), ("effect", "Effects")):
            ms = [m for m in members if m.get("kind") == kind]
            if ms:
                lines.append("%s (%d)" % (title, len(ms)))
                lines += ["  %-24s %s" % (m["name"], m.get("category", "")) for m in ms]
                lines.append("")
        _write(os.path.join(top, "PLUGINS.txt"), "\n".join(lines))
        name = all_name(suite, version, family)
        write_zip(top, os.path.join(out_dir, name))
        written.append(name)
    return written


def _parse_inputs(values):
    out = []
    for v in values:
        label, sep, folder = v.partition("=")
        if not sep:
            label, folder = "", v
        out.append((label, folder))
    return out


# ------------------------------------------------------------ self test ----

def _fake_assets(root):
    """The app's assets folder in small: what plugins read and what they never do."""
    for rel in ("fonts/Inter-Regular.ttf", "i18n/de.po", "i18n/gridline.pot", "sounds/prism/open.wav", "templates/a.json",
                "scripts/audio/fade.lua", "scripts/project/tidy.lua", "demo/Gridline.gridline.json", "icons/ui/x.svg"):
        _write(os.path.join(root, *rel.split("/")), "x")


def _fake_bundle(folder, suite_name, fmt, family, arch):
    """A small stand-in for a built bundle (the layout matters, not the code)."""
    p = os.path.join(folder, suite_name + "." + fmt)
    if family == "macos" or fmt == "vst3":
        exe_dir = os.path.join(p, "Contents", "MacOS" if family == "macos" else arch)
        os.makedirs(exe_dir, exist_ok=True)
        binary = os.path.join(exe_dir, suite_name + ("" if family == "macos" else ".so"))
        with open(binary, "wb") as f:
            f.write(b"\x7fELF engine\x00GridlineSuiteAuGLViewFactory_T0000000000000000\x00")
        os.chmod(binary, 0o755)
        _fake_assets(os.path.join(p, "Contents", "Resources", "assets"))
        if family == "macos":
            pl = {"CFBundleIdentifier": "org.gridline.GridlineSuite." + fmt, "CFBundleExecutable": suite_name}
            if fmt == "component":
                pl["AudioComponents"] = [{"type": "aumu", "subtype": "plas", "manufacturer": "GrLn"},
                                         {"type": "aufx", "subtype": "revb", "manufacturer": "GrLn"},
                                         {"type": "aufx", "subtype": "hiht", "manufacturer": "GrLn"},
                                         {"type": "aufx", "subtype": "edsn", "manufacturer": "GrLn"}]
            with open(os.path.join(p, "Contents", "Info.plist"), "wb") as f:
                plistlib.dump(pl, f)
    else:
        os.makedirs(folder, exist_ok=True)
        with open(p, "wb") as f:
            f.write(b"engine")
        os.chmod(p, 0o755)
        _fake_assets(os.path.join(folder, suite_name + "-assets"))


def self_test():
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    check(slug("Hi-hat") == "HiHat" and slug("Turntable FX (old)") == "TurntableFXOld" and slug("FM Synth") == "FMSynth", "slugs")
    check(slug("Acid Bass") == "AcidBass" and slug("Plasma") == "Plasma", "plain slugs")
    cat = [{"type": "gl.plasma", "name": "Plasma", "kind": "instrument", "category": "Synth", "au": "aumu/plas/GrLn"},
           {"type": "gl.reverb", "name": "Reverb", "kind": "effect", "category": "Reverb", "au": "aufx/revb/GrLn"},
           {"type": "gl.hihat", "name": "Hi-hat", "kind": "instrument", "category": "Drums", "au": "aufx/hiht/GrLn"},
           {"type": "gl.edison", "name": "Loupe", "kind": "effect", "category": "Misc", "au": "aufx/edsn/GrLn"}]
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import downloads  # the release page reads these names
    with tempfile.TemporaryDirectory() as tmp:
        layouts = {"macos": [("", "")], "windows": [("x64", "x86_64-win"), ("arm64", "arm64-win")],
                   "linux": [("x86_64", "x86_64-linux"), ("aarch64", "aarch64-linux")],
                   "bsd": [("freebsd-x86_64", "x86_64-freebsd"), ("netbsd-x86_64", "x86_64-netbsd")]}
        for family, procs in layouts.items():
            inputs = []
            for label, arch in procs:
                folder = os.path.join(tmp, "in", family, label or "universal")
                fmts = ("vst3", "clap", "component") if family == "macos" else ("vst3", "clap")
                for fmt in fmts:
                    _fake_bundle(folder, "Gridline Suite", fmt, family, arch)
                inputs.append((label, folder))
            out = os.path.join(tmp, "out", family)
            lic = os.path.join(tmp, "LICENSE")
            _write(lic, "licence text\n")
            release = package("gridline", family, "0.8.0", cat, inputs, os.path.join(tmp, "release", family), lic)
            check(release == ["Gridline-Suite-0.8.0-%s-all.zip" % family], family + ": a release writes the all zip alone: %s" % release)
            for n in release:
                e = downloads.classify(n, "0.8.0")
                check(e is not None and e.get("product") == "suite", family + ": the release page places " + n)
            # Single-plugin bundles stay for tests (the Objective-C class test loads two of them).
            names = package("gridline", family, "0.8.0", cat, inputs, out, lic, singles=True)
            check(len(names) == 5, family + ": with --singles, four singles and one all zip")
            for n in names:
                if n not in release:
                    check(downloads.classify(n, "0.8.0") is None, family + ": the release page does not place " + n)
            check("Gridline-Suite-HiHat-0.8.0-%s.zip" % family in names, family + ": single name")
            check("Gridline-Suite-0.8.0-%s-all.zip" % family in names, family + ": all name")
            with zipfile.ZipFile(os.path.join(out, "Gridline-Suite-Plasma-0.8.0-%s.zip" % family)) as z:
                listing = z.namelist()
                lists = [n for n in listing if n.endswith(MEMBER_LIST)]
                check(lists and all(z.read(n).decode().splitlines()[1:] == ["suite gridline", "gl.plasma"] for n in lists),
                      family + ": member lists name the plugin alone")
                check(all(n.startswith("Plasma/") for n in listing), family + ": one top folder")
                check("Plasma/README.txt" in listing and "Plasma/LICENSE.txt" in listing, family + ": readme and licence")
                if family == "macos":
                    check("Plasma/Components/Plasma.component/Contents/Info.plist" in listing, "macos: AU bundle renamed")
                    pl = plistlib.loads(z.read("Plasma/Components/Plasma.component/Contents/Info.plist"))
                    check([c["subtype"] for c in pl["AudioComponents"]] == ["plas"], "macos: the AU lists only its own component")
                    check(pl["CFBundleIdentifier"] == "org.gridline.GridlineSuite.Plasma.component", "macos: own bundle id")
                    check("Plasma/VST3/Gridline Suite/Plasma.vst3/Contents/Resources/" + MEMBER_LIST in listing, "macos: VST3 list")
                elif family in ("windows", "linux"):
                    a, b = layouts[family][0][1], layouts[family][1][1]
                    for arch in (a, b):
                        check("Plasma/VST3/Gridline Suite/Plasma.vst3/Contents/%s/Plasma.so" % arch in listing,
                              family + ": one VST3 bundle holds " + arch + " with its binary renamed")
                    lab = layouts[family][0][0]
                    clap = "Plasma/CLAP/%s/Gridline Suite/Plasma.clap" % lab
                    check(clap in listing and "Plasma/CLAP/%s/Gridline Suite/Plasma-assets/%s" % (lab, MEMBER_LIST) in listing,
                          family + ": CLAP renamed with its assets folder and list")
                    mode = (z.getinfo(clap).external_attr >> 16) & 0o777
                    check(mode == 0o755, family + ": execute bit kept")
                else:
                    check("Plasma/freebsd-x86_64/VST3/Gridline Suite/Plasma.vst3/Contents/x86_64-freebsd/Plasma.so" in listing and
                          "Plasma/netbsd-x86_64/CLAP/Gridline Suite/Plasma.clap" in listing, "bsd: one folder per system")
            with zipfile.ZipFile(os.path.join(out, "Gridline-Suite-0.8.0-%s-all.zip" % family)) as z:
                listing = z.namelist()
                check("Gridline Suite/PLUGINS.txt" in listing, family + ": PLUGINS.txt")
                lists = [n for n in listing if n.endswith(MEMBER_LIST)]
                check(lists and all(z.read(n).decode().count("gl.") == 4 for n in lists), family + ": the all zip lists every member")
                check(any("/scripts/audio/fade.lua" in n for n in listing), family + ": the all zip keeps Loupe's scripts")
                if family == "macos":
                    exe = "Gridline Suite/Components/Gridline Suite.component/Contents/MacOS/Gridline Suite"
                    check(b"Factory_T0000000000000000" in z.read(exe), "macos: the whole suite's AU keeps its factory name")
            # assets: only what plugins read
            tags = set()
            for plug in ("Plasma", "Loupe"):
                with zipfile.ZipFile(os.path.join(out, "Gridline-Suite-%s-0.8.0-%s.zip" % (plug, family))) as z:
                    listing = z.namelist()
                    files = [n for n in listing if not n.endswith("/")]
                    check(any(n.endswith("/fonts/Inter-Regular.ttf") for n in files) and any(n.endswith("/i18n/de.po") for n in files),
                          family + ": " + plug + " keeps the fonts and translations")
                    gone = [n for n in files if any(x in n for x in ("/sounds/", "/templates/", "/demo/", "/icons/", ".pot", "/scripts/project/"))]
                    check(not gone, family + ": " + plug + " leaves out what plugins never read: " + ", ".join(gone[:3]))
                    has_scripts = any("/scripts/audio/fade.lua" in n for n in files)
                    check(has_scripts == (plug == "Loupe"), family + ": only Loupe keeps the audio scripts")
                    if family == "macos":
                        exe = "%s/Components/%s.component/Contents/MacOS/Gridline Suite" % (plug, plug)
                        data = z.read(exe)
                        m = re.search(rb"Factory_T([0-9a-f]{16})", data)
                        check(m is not None and m.group(1) != b"0" * 16, "macos: " + plug + "'s AU factory has its own name")
                        if m:
                            tags.add(m.group(1))
            if family == "macos":
                check(len(tags) == 2, "macos: two single plugins, two factory names")
            # reproducible
            again = os.path.join(tmp, "again", family)
            package("gridline", family, "0.8.0", cat, inputs, again, lic, singles=True)
            for n in names:
                with open(os.path.join(out, n), "rb") as f1, open(os.path.join(again, n), "rb") as f2:
                    check(f1.read() == f2.read(), family + ": the same zip twice: " + n)
        # Mini names
        check(single_name("mini", "Trim", "0.8.0", "macos") == "Gridline-Suite-Mini-Trim-0.8.0-macos.zip", "mini single name")
        check(all_name("mini", "0.8.0", "bsd") == "Gridline-Suite-Mini-0.8.0-bsd-all.zip", "mini all name")
        check(downloads.classify("Gridline-Suite-Mini-Trim-0.8.0-macos.zip", "0.8.0") is None, "no Mini single zip on the release page")
        e = downloads.classify("Gridline-Suite-Mini-0.8.0-bsd-all.zip", "0.8.0")
        check(e is not None and e.get("product") == "mini", "the release page places the Mini all zip")
    for f in fails:
        print("FAIL", f)
    print("package-plugins self-test: %s" % ("ok" if not fails else "%d failed" % len(fails)))
    return 1 if fails else 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--suite", choices=sorted(SUITES), required=True)
    ap.add_argument("--family", choices=FAMILIES, required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--catalogue", required=True, help="gridline suite-info <suite> --json")
    ap.add_argument("--input", action="append", required=True, help="[LABEL=]folder with the suite's bundles")
    ap.add_argument("--out", required=True)
    ap.add_argument("--license")
    ap.add_argument("--sign", action="store_true", help="macOS: re-sign changed bundles ad hoc")
    ap.add_argument("--only", action="append", help="only these plugin types (testing)")
    ap.add_argument("--singles", action="store_true", help="also single-plugin zips (tests only, never released)")
    ap.add_argument("--singles-only", action="store_true", help="single-plugin zips and no all-plugins zip (tests only)")
    a = ap.parse_args(argv[1:])
    with open(a.catalogue, encoding="utf-8") as f:
        catalogue = json.load(f)
    names = package(a.suite, a.family, a.version, catalogue, _parse_inputs(a.input), a.out, a.license, a.sign, set(a.only or []), a.singles, a.singles_only)
    for n in names:
        print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
