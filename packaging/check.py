#!/usr/bin/env python3
"""K-1: checks the distribution channel files in packaging/ (ctest packaging_manifests, CI).

    python3 packaging/check.py [--schemas <dir>] [--rendered <dir>]

* every template renders (render.py --fake-sums into a temporary folder) with no placeholder left;
* the rendered files parse (JSON, XML; YAML when PyYAML is installed) and carry the project
  version from CMakeLists.txt;
* the Flatpak manifest agrees with the rest of the tree: application id = AppStream id = file
  name, FFmpeg version, checksum and configure options = tools/release/media-tools.json and
  tools/release/fetch-media-tools.py, the yt-dlp version, the plugin extension version = the
  runtime version, sandbox permissions Flathub's linter refuses;
* the AppStream metadata names the Flatpak id and replaces the old one; the Windows manifests
  name the installer's AppId (tools/windows/gridline.iss);
* K-119, the file types Gridline opens: the Linux MIME package and desktop entry (and, where
  installed, update-mime-database and desktop-file-validate on them), the macOS Info.plist's
  document and type declarations (plutil -lint where installed) and the Windows installer's
  "Open with" entries; Gridline never takes another program's file type as its default;
* with --schemas <dir>: the Flatpak, Snapcraft, Scoop and the three Windows package manager
  manifests validate against their published JSON schemas (needs the jsonschema module; CI
  downloads the schemas, see .github/workflows/packaging.yml).
--rendered <dir> checks an existing render.py output instead of rendering one.
Exit status 0 when everything holds; problems are printed one per line.
"""
import argparse
import importlib.util
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True  # importing render.py and fetch-media-tools.py leaves no __pycache__

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP_ID = "com.lolitemaultes.Gridline"
OLD_ID = "org.gridline.Gridline"

try:
    import yaml  # PyYAML (optional)
except ImportError:
    yaml = None

problems = []
notes = []


def bad(msg):
    problems.append(msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_yaml(path):
    if yaml is None:
        return None
    try:
        return yaml.safe_load(read(path))
    except yaml.YAMLError as e:
        bad(f"{path}: YAML does not parse: {e}")
        return None


def check_flatpak(render_mod):
    path = os.path.join(HERE, "flatpak", f"{APP_ID}.yml")
    text = read(path)
    m = load_yaml(path)
    media = json.loads(read(os.path.join(ROOT, "tools", "release", "media-tools.json")))
    fetch = load_module("fetch_media_tools", os.path.join(ROOT, "tools", "release", "fetch-media-tools.py"))
    # Text checks (also without PyYAML).
    if f"id: {APP_ID}\n" not in text:
        bad(f"{path}: id is not {APP_ID}")
    if media["ffmpeg"]["url"] not in text or media["ffmpeg"]["sha256"] not in text:
        bad(f"{path}: FFmpeg URL / SHA-256 differ from tools/release/media-tools.json")
    ytv = media["yt-dlp"]["version"]
    if f"/releases/download/{ytv}/yt-dlp.tar.gz" not in text:
        bad(f"{path}: yt-dlp is not version {ytv} (tools/release/media-tools.json)")
    for wm in ("SDL_VIDEO_X11_WMCLASS", "SDL_VIDEO_WAYLAND_WMCLASS"):
        if f"--env={wm}={APP_ID}" not in text:
            bad(f"{path}: {wm} must be the application id")
    if m is None:
        notes.append("PyYAML not installed: Flatpak manifest checked as text only")
        return
    if m.get("id") != APP_ID:
        bad(f"{path}: id {m.get('id')} != {APP_ID}")
    rv = str(m.get("runtime-version"))
    ext = (m.get("add-extensions") or {}).get("org.freedesktop.LinuxAudio.Plugins") or {}
    if str(ext.get("version")) != rv:
        bad(f"{path}: LinuxAudio.Plugins extension version {ext.get('version')} != runtime-version {rv}")
    fa = m.get("finish-args") or []
    if "--socket=x11" in fa and "--socket=fallback-x11" in fa:
        bad(f"{path}: x11 and fallback-x11 together (Flathub linter error)")
    if any(a in fa for a in ("--socket=x11", "--socket=fallback-x11")) and "--share=ipc" not in fa:
        bad(f"{path}: X11 without --share=ipc (Flathub linter error)")
    if "--device=all" in fa and any(a.startswith("--device=") and a != "--device=all" for a in fa):
        bad(f"{path}: --device=all with another --device (redundant)")
    if "--filesystem=host" in fa and "--filesystem=home" in fa:
        bad(f"{path}: home and host together (Flathub linter error)")
    for a in fa:
        if a in ("--socket=session-bus", "--socket=system-bus") or a.startswith("--filesystem=/home"):
            bad(f"{path}: {a} is refused by Flathub's linter")
    mods = {mod.get("name"): mod for mod in m.get("modules", []) if isinstance(mod, dict)}
    ff = mods.get("ffmpeg-tools")
    if not ff:
        bad(f"{path}: no ffmpeg-tools module")
    else:
        want = ["--bindir=/app/share/gridline/tools"] + fetch.configure_flags(["--enable-zlib"])
        if ff.get("config-opts") != want:
            bad(f"{path}: FFmpeg's config-opts differ from tools/release/fetch-media-tools.py (configure_flags + --enable-zlib)")
    gl = mods.get("gridline")
    if not gl:
        bad(f"{path}: no gridline module")
    else:
        src = gl.get("sources") or [{}]
        if src[0].get("type") != "dir":
            bad(f"{path}: the in-tree manifest builds this source tree (type: dir); render.py pins the tag for Flathub")
    # The Flathub copy render.py makes keeps the same modules and pins a git commit.
    fake = "0123456789abcdef0123456789abcdef01234567"
    fl = yaml.safe_load(render_mod.flathub_manifest(text, "9.9.9", fake))
    gsrc = [mod for mod in fl["modules"] if isinstance(mod, dict) and mod.get("name") == "gridline"][0]["sources"][0]
    if gsrc.get("type") != "git" or gsrc.get("tag") != "v9.9.9" or gsrc.get("commit") != fake:
        bad("render.py: the Flathub manifest does not pin the release tag and commit")
    if [mm.get("name") for mm in fl["modules"]] != [mm.get("name") for mm in m["modules"]]:
        bad("render.py: the Flathub manifest lost a module")


def check_metainfo():
    path = os.path.join(ROOT, "assets", "linux", f"{APP_ID}.metainfo.xml")
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as e:
        bad(f"{path}: {e}")
        return
    if root.findtext("id") != APP_ID:
        bad(f"{path}: <id> is not {APP_ID}")
    if OLD_ID not in [e.text for e in root.findall("replaces/id")]:
        bad(f"{path}: <replaces> does not list {OLD_ID}")
    summary = root.findtext("summary") or ""
    if not 0 < len(summary) <= 35 or summary.endswith("."):
        bad(f"{path}: summary should be 1-35 characters without a full stop (Flathub)")
    if root.find("content_rating") is None or root.find("developer") is None:
        bad(f"{path}: content_rating and developer are required by Flathub")
    if not root.findall("screenshots/screenshot/image"):
        bad(f"{path}: no screenshots")
    if root.find("branding") is None:
        bad(f"{path}: no branding colours")
    for tag in ("homepage", "bugtracker", "vcs-browser"):
        if not root.findall(f"url[@type='{tag}']"):
            bad(f"{path}: no <url type=\"{tag}\">")
    cmake = read(os.path.join(ROOT, "CMakeLists.txt"))
    if f"assets/linux/{APP_ID}.metainfo.xml DESTINATION share/metainfo" not in cmake:
        bad("CMakeLists.txt does not install the AppStream metadata under its id")
    for f in os.listdir(os.path.join(ROOT, "assets", "linux")):
        if f.endswith(".metainfo.xml") and f != f"{APP_ID}.metainfo.xml":
            bad(f"assets/linux/{f}: a second metainfo file")


MIME_NS = "{http://www.freedesktop.org/standards/shared-mime-info}"
# Gridline's own types: {glob: weight at least}, the format it is written in. The single extensions
# (.gridline, .gridlinez) are the names Gridline saves; the two-part names are those of projects
# saved by earlier versions, which must keep winning over the plain *.json / *.zip (weight 50).
OWN_TYPES = {
    "application/x-gridline-project": ({"*.gridline": 80, "*.gridline.json": 60}, "application/json"),
    "application/x-gridline-zipped-project": ({"*.gridlinez": 80, "*.gridline.zip": 60}, "application/zip"),
}
# Other formats Gridline opens as projects: their standard types, listed for Open with (never
# defined here, so Gridline is never the only program that claims them).
OPENED_TYPES = ["audio/x-xm", "audio/x-mod"]


def desktop_keys(path):
    keys = {}
    group = None
    for line in read(path).splitlines():
        if line.startswith("["):
            group = line.strip()
        elif "=" in line and group == "[Desktop Entry]":
            k, v = line.split("=", 1)
            keys[k] = v
    return keys


def check_linux_file_types():
    path = os.path.join(ROOT, "assets", "linux", "gridline.xml")
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as e:
        bad(f"{path}: {e}")
        return
    defined = {t.get("type"): t for t in root.findall(f"{MIME_NS}mime-type")}
    for t in defined:
        if t not in OWN_TYPES:
            bad(f"{path}: defines {t}: only Gridline's own formats belong here")
    for t, (want_globs, parent) in OWN_TYPES.items():
        e = defined.get(t)
        if e is None:
            bad(f"{path}: {t} is missing")
            continue
        globs = {g.get("pattern"): int(g.get("weight", "50")) for g in e.findall(f"{MIME_NS}glob")}
        for glob, weight in want_globs.items():
            if globs.get(glob, 0) < weight:
                bad(f"{path}: {t} needs the glob {glob} with a weight of {weight} or more (it must win over the plain format)")
        for glob in globs:
            if glob not in want_globs:
                bad(f"{path}: {t} has the glob {glob}: only Gridline's own names belong here")
        if parent not in [c.get("type") for c in e.findall(f"{MIME_NS}sub-class-of")]:
            bad(f"{path}: {t} is not a sub-class-of {parent}")
        if not e.findtext(f"{MIME_NS}comment"):
            bad(f"{path}: {t} has no comment")
        icon = e.find(f"{MIME_NS}icon")
        if icon is None or icon.get("name") != "gridline":
            bad(f"{path}: {t} must use the icon 'gridline' (the Flatpak build renames it)")
    desktop = os.path.join(ROOT, "assets", "linux", "gridline.desktop")
    keys = desktop_keys(desktop)
    if "%F" not in keys.get("Exec", "") and "%U" not in keys.get("Exec", ""):
        bad(f"{desktop}: Exec does not take the files to open (%F)")
    mimes = [m for m in keys.get("MimeType", "").split(";") if m]
    for m in list(OWN_TYPES) + OPENED_TYPES:
        if m not in mimes:
            bad(f"{desktop}: MimeType does not list {m}")
    for m in mimes:
        if m not in OWN_TYPES and m not in OPENED_TYPES:
            bad(f"{desktop}: MimeType lists {m}: only types Gridline opens as projects (see OPENED_TYPES)")
    brand = read(os.path.join(ROOT, "tools", "make_brand.py"))
    if read(desktop) not in brand:
        bad("tools/make_brand.py: its desktop entry differs from assets/linux/gridline.desktop")
    if shutil.which("desktop-file-validate"):
        r = subprocess.run(["desktop-file-validate", desktop], capture_output=True, text=True)
        if r.returncode != 0 or "error" in (r.stdout + r.stderr).lower():
            bad(f"desktop-file-validate {desktop}: {r.stdout}{r.stderr}")
        else:
            notes.append("desktop-file-validate: gridline.desktop is valid")
    else:
        notes.append("desktop-file-validate not installed: desktop entry validation skipped")
    if shutil.which("update-mime-database"):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "mime", "packages"))
            shutil.copy(path, os.path.join(tmp, "mime", "packages", "gridline.xml"))
            r = subprocess.run(["update-mime-database", os.path.join(tmp, "mime")], capture_output=True, text=True)
            # (a scratch folder outside XDG_DATA_DIRS gets a note on stderr: only the status counts)
            if r.returncode != 0:
                bad(f"update-mime-database refuses {path}: {r.stdout}{r.stderr}")
            else:
                globs2 = read(os.path.join(tmp, "mime", "globs2"))
                subclasses = read(os.path.join(tmp, "mime", "subclasses"))
                for t, (want_globs, parent) in OWN_TYPES.items():
                    for glob, weight in want_globs.items():
                        m = re.search(rf"^(\d+):{re.escape(t)}:{re.escape(glob)}$", globs2, re.M)
                        if not m or int(m.group(1)) < weight:
                            bad(f"update-mime-database: no weight {weight} glob {glob} for {t}")
                    if f"{t} {parent}\n" not in subclasses:
                        bad(f"update-mime-database: {t} is not recorded as a sub-class of {parent}")
                notes.append("update-mime-database: gridline.xml compiles")
    else:
        notes.append("update-mime-database not installed: MIME compile check skipped")
    cmake = read(os.path.join(ROOT, "CMakeLists.txt"))
    for needle in ("assets/linux/gridline.xml DESTINATION share/mime/packages",
                   "assets/linux/gridline.desktop DESTINATION share/applications",
                   "CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/cmake/linux/rpm-refresh-caches.sh"):
        if needle not in cmake:
            bad(f"CMakeLists.txt: missing '{needle}'")
    port = read(os.path.join(HERE, "freebsd", "audio", "gridline", "Makefile.in"))
    uses = re.search(r"^USES=((?:.*\\\n)*.*)$", port, re.M)
    for u in ("desktop-file-utils", "shared-mime-info"):
        if not uses or u not in uses.group(1).replace("\\", " ").split():
            bad(f"freebsd Makefile.in: USES lacks {u} (the package refreshes the desktop caches)")


def render_plist(numeric):
    path = os.path.join(ROOT, "cmake", "macos", "Info.plist.in")
    text = read(path).replace("@GRIDLINE_VERSION_NUMERIC@", numeric).replace("@GL_MACOS_MIN@", "11.0")
    left = re.findall(r"@[A-Za-z0-9_]+@", text)
    if left:
        bad(f"{path}: placeholders the build does not set: {left}")
    return path, text


def check_macos_file_types(forms):
    if not os.path.isfile(os.path.join(ROOT, "cmake", "macos", "Info.plist.in")):
        bad("cmake/macos/Info.plist.in is missing (Gridline.app's Info.plist with its file types)")
        return
    path, text = render_plist(forms["numeric"])
    try:
        pl = plistlib.loads(text.encode("utf-8"))
    except Exception as e:  # plistlib raises several kinds
        bad(f"{path}: does not parse: {e}")
        return
    if "configure_file(${CMAKE_SOURCE_DIR}/cmake/macos/Info.plist.in ${GRIDLINE_APP_BUNDLE}/Contents/Info.plist @ONLY)" not in \
            read(os.path.join(ROOT, "CMakeLists.txt")):
        bad("CMakeLists.txt does not write Gridline.app's Info.plist from cmake/macos/Info.plist.in")
    for k in ("CFBundleIdentifier", "CFBundleExecutable", "CFBundleShortVersionString", "LSMinimumSystemVersion"):
        if not pl.get(k):
            bad(f"{path}: no {k}")
    # The extension Gridline saves first (the type's preferred one), then the name earlier versions saved.
    own = {"com.lolitemaultes.gridline.project": (["gridline", "gridline.json"], "application/x-gridline-project", "public.json"),
           "com.lolitemaultes.gridline.zipped-project": (["gridlinez", "gridline.zip"], "application/x-gridline-zipped-project",
                                                          "public.zip-archive")}
    imported = {"com.lolitemaultes.gridline.xm-module": ("xm", "audio/x-xm"),
                "com.lolitemaultes.gridline.mod-module": ("mod", "audio/x-mod")}
    exported = {d.get("UTTypeIdentifier"): d for d in pl.get("UTExportedTypeDeclarations", [])}
    if set(exported) != set(own):
        bad(f"{path}: exported types {sorted(exported)} are not Gridline's own {sorted(own)}")
    for uti, (exts, mime, parent) in own.items():
        d = exported.get(uti, {})
        tags = d.get("UTTypeTagSpecification", {})
        if tags.get("public.filename-extension") != exts or tags.get("public.mime-type") != [mime]:
            bad(f"{path}: {uti} should carry the extensions {exts} (in that order) and the type {mime}")
        if parent not in d.get("UTTypeConformsTo", []):
            bad(f"{path}: {uti} does not conform to {parent}")
        if mime not in OWN_TYPES:
            bad(f"{path}: {uti}'s type {mime} is not one of the Linux MIME package's")
    imp = {d.get("UTTypeIdentifier"): d for d in pl.get("UTImportedTypeDeclarations", [])}
    for uti, (ext, mime) in imported.items():
        tags = imp.get(uti, {}).get("UTTypeTagSpecification", {})
        if tags.get("public.filename-extension") != [ext] or tags.get("public.mime-type") != [mime]:
            bad(f"{path}: imported type {uti} should carry the extension {ext} and the type {mime}")
    handled = {}
    for d in pl.get("CFBundleDocumentTypes", []):
        if not d.get("CFBundleTypeName"):
            bad(f"{path}: a document type without CFBundleTypeName")
        for uti in d.get("LSItemContentTypes", []):
            handled[uti] = (d.get("CFBundleTypeRole"), d.get("LSHandlerRank"))
        if "CFBundleTypeExtensions" in d:
            bad(f"{path}: {d.get('CFBundleTypeName')}: name types by UTI (LSItemContentTypes), not by extension")
    for uti in own:
        if handled.get(uti) != ("Editor", "Owner"):
            bad(f"{path}: {uti} should be handled as Editor, Owner (got {handled.get(uti)})")
    for uti in list(imported) + ["public.json", "public.zip-archive"]:
        if handled.get(uti) != ("Viewer", "Alternate"):
            bad(f"{path}: {uti} should be handled as Viewer, Alternate (got {handled.get(uti)})")
    for uti, (role, rank) in handled.items():
        if uti not in own and rank != "Alternate":
            bad(f"{path}: {uti} belongs to another program: rank Alternate only (got {rank})")
        if "flp" in uti.lower():
            bad(f"{path}: {uti}: another music program's project type is not declared")
    if shutil.which("plutil"):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "Info.plist")
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)
            r = subprocess.run(["plutil", "-lint", out], capture_output=True, text=True)
            if r.returncode != 0:
                bad(f"plutil -lint: {r.stdout}{r.stderr}")
            else:
                notes.append("plutil -lint: Info.plist is valid")


def check_windows_file_types():
    path = os.path.join(ROOT, "tools", "windows", "gridline.iss")
    text = read(path)
    if not re.search(r"^ChangesAssociations=yes$", text, re.M):
        bad(f"{path}: ChangesAssociations=yes is missing (Explorer would not see the new entries)")
    tasks = {m.group(1): m.group(2) for m in re.finditer(r'^Name: "(assoc\w+)";(.*)$', text, re.M)}
    for t in ("assocproject", "assocmodule", "assocflp"):
        if t not in tasks:
            bad(f"{path}: task {t} is missing")
    if "assocflp" in tasks and "unchecked" not in tasks["assocflp"]:
        bad(f"{path}: the FL Studio project entry must stay off by default")
    sec = re.search(r"^\[Registry\]\n(.*?)(?=^\[)", text, re.M | re.S)
    if not sec:
        bad(f"{path}: no [Registry] section")
        return
    entries = []
    for line in sec.group(1).splitlines():
        if not line.startswith("Root:"):
            continue
        fields = dict((k.strip(), v.strip()) for k, v in (p.split(":", 1) for p in line.split("; ") if ":" in p))
        entries.append((fields.get("Subkey", "").strip('"'), fields.get("ValueName", "").strip('"'),
                        fields.get("ValueData", ""), fields.get("Tasks", "")))
    # Gridline's own extensions: Gridline is their default program (only Gridline writes them).
    own_exts = {".gridline": "Gridline.Project", ".gridlinez": "Gridline.ZippedProject"}
    progids, defaults = {}, {}
    for sub, name, data, task in entries:
        m = re.fullmatch(r"Software\\Classes\\(\.[A-Za-z0-9]+)(\\.*)?", sub)
        if m and m.group(1) in own_exts and m.group(2) is None:
            if name == "":
                defaults[m.group(1)] = data.strip('"')
            elif name != "Content Type":
                bad(f"{path}: writes {sub} value {name}: only its default program and Content Type")
            if task != "assocproject":
                bad(f"{path}: {sub} is written without the task assocproject")
        elif m and m.group(2) != "\\OpenWithProgids":
            bad(f"{path}: writes {sub}: an extension's own key (its default program) is the user's choice")
        if m and m.group(2) == "\\OpenWithProgids":
            progids.setdefault(m.group(1), []).append(name)
    for ext, pid in own_exts.items():
        if defaults.get(ext) != pid:
            bad(f"{path}: {ext} should open with {pid} by default (got {defaults.get(ext)})")
    want = {".gridline": "Gridline.Project", ".gridlinez": "Gridline.ZippedProject", ".json": "Gridline.Project",
            ".zip": "Gridline.ZippedProject", ".xm": "Gridline.Module", ".mod": "Gridline.Module"}
    for ext, pid in want.items():
        if pid not in progids.get(ext, []):
            bad(f"{path}: {ext} does not list {pid} in OpenWithProgids")
    for pid in set(n for names in progids.values() for n in names):
        cmd = [d for s2, n, d, _ in entries if s2 == f"Software\\Classes\\{pid}\\shell\\open\\command"]
        if not cmd or '""%1""' not in cmd[0] or '""{app}\\gridline-app.exe""' not in cmd[0]:
            bad(f"{path}: {pid} has no open command passing the quoted file (\"%1\")")
    supported = [n for s2, n, _, _ in entries if s2 == "Software\\Classes\\Applications\\gridline-app.exe\\SupportedTypes"]
    for ext in want:
        if ext not in supported:
            bad(f"{path}: Applications\\gridline-app.exe\\SupportedTypes lacks {ext}")


def check_file_types(forms):
    check_linux_file_types()
    check_macos_file_types(forms)
    check_windows_file_types()


def check_rendered(out, forms):
    v = forms["full"]
    placeholder = re.compile(r"@[A-Z0-9_]+(?::[A-Za-z0-9._-]+)?@")
    for dirpath, _, files in os.walk(out):
        for f in files:
            p = os.path.join(dirpath, f)
            try:
                text = read(p)
            except UnicodeDecodeError:
                continue
            for mm in placeholder.finditer(text):
                bad(f"{os.path.relpath(p, out)}: placeholder {mm.group(0)} left")
    # Each templated file names the project version.
    expect = {
        "aur/gridline-bin/PKGBUILD": f"_ver={v}\n",
        "aur/gridline/PKGBUILD": f"_ver={v}\n",
        "aur/gridline-bin/.SRCINFO": f"\tpkgver = {forms['pkgver']}\n",
        "aur/gridline/.SRCINFO": f"\tpkgver = {forms['pkgver']}\n",
        "homebrew/Casks/gridline.rb": f'version "{v}"',
        f"winget/manifests/g/Gridline/Gridline/{v}/Gridline.Gridline.yaml": f"PackageVersion: {v}\n",
        "scoop/gridline.json": f'"version": "{v}"',
        "chocolatey/gridline/gridline.nuspec": f"<version>{v}</version>",
        "freebsd/audio/gridline/Makefile": f"DISTVERSION=\t{v}\n",
        "copr/gridline.spec": f"Version:        {forms['numeric']}\n",
    }
    for rel, needle in expect.items():
        p = os.path.join(out, rel)
        if not os.path.isfile(p):
            bad(f"render.py did not write {rel}")
        elif needle not in read(p):
            bad(f"{rel}: does not carry version {v} ({needle.strip()})")
    if "-" in forms["pkgver"]:
        bad(f"Arch pkgver {forms['pkgver']} has a dash")
    try:
        json.loads(read(os.path.join(out, "scoop/gridline.json")))
    except (OSError, ValueError) as e:
        bad(f"scoop/gridline.json: {e}")
    try:
        ET.parse(os.path.join(out, "chocolatey/gridline/gridline.nuspec"))
    except (OSError, ET.ParseError) as e:
        bad(f"chocolatey/gridline/gridline.nuspec: {e}")
    iss = read(os.path.join(ROOT, "tools", "windows", "gridline.iss"))
    app_id = re.search(r"^AppId=\{(\{[0-9A-F-]+\})", iss, re.M)
    inst = os.path.join(out, f"winget/manifests/g/Gridline/Gridline/{v}/Gridline.Gridline.installer.yaml")
    if not app_id or f"ProductCode: '{app_id.group(1)}_is1'" not in read(inst):
        bad("winget: ProductCode is not the installer's AppId + _is1 (tools/windows/gridline.iss)")
    if yaml is not None:
        for rel in [f"winget/manifests/g/Gridline/Gridline/{v}/{n}" for n in
                    ("Gridline.Gridline.yaml", "Gridline.Gridline.installer.yaml", "Gridline.Gridline.locale.en-US.yaml")] + ["snap/snapcraft.yaml"]:
            load_yaml(os.path.join(out, rel))


def check_snap():
    path = os.path.join(HERE, "snap", "snapcraft.yaml")
    text = read(path)
    if "tools/release/version.sh" not in text or "craftctl set version" not in text:
        bad(f"{path}: the version must come from CMakeLists.txt (tools/release/version.sh)")
    if f"common-id: {APP_ID}" not in text:
        bad(f"{path}: common-id is not {APP_ID}")
    load_yaml(path)


def check_schemas(schemas, out, forms):
    try:
        import jsonschema
    except ImportError:
        notes.append("jsonschema not installed: schema checks skipped")
        return
    if yaml is None:
        notes.append("PyYAML not installed: schema checks skipped")
        return
    v = forms["full"]
    wdir = f"winget/manifests/g/Gridline/Gridline/{v}"
    pairs = [
        ("flatpak-manifest.schema.json", os.path.join(HERE, "flatpak", f"{APP_ID}.yml")),
        ("flatpak-manifest.schema.json", os.path.join(out, "flathub", f"{APP_ID}.yml")),
        ("snapcraft.json", os.path.join(HERE, "snap", "snapcraft.yaml")),
        ("scoop.schema.json", os.path.join(out, "scoop", "gridline.json")),
        ("winget.version.json", os.path.join(out, wdir, "Gridline.Gridline.yaml")),
        ("winget.installer.json", os.path.join(out, wdir, "Gridline.Gridline.installer.yaml")),
        ("winget.defaultLocale.json", os.path.join(out, wdir, "Gridline.Gridline.locale.en-US.yaml")),
    ]
    for schema_name, doc_path in pairs:
        sp = os.path.join(schemas, schema_name)
        if not os.path.isfile(sp):
            bad(f"schema {schema_name} missing in {schemas}")
            continue
        schema = json.loads(read(sp))
        doc = yaml.safe_load(read(doc_path))
        cls = jsonschema.validators.validator_for(schema)
        errors = sorted(cls(schema).iter_errors(doc), key=lambda e: list(e.path))
        for e in errors[:10]:
            bad(f"{os.path.basename(doc_path)} ({schema_name}): {'/'.join(str(x) for x in e.path)}: {e.message[:200]}")
        if not errors:
            notes.append(f"{os.path.basename(doc_path)}: valid ({schema_name})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--schemas", help="folder with the JSON schemas (see .github/workflows/packaging.yml)")
    ap.add_argument("--rendered", help="an existing render.py output")
    a = ap.parse_args()
    render_mod = load_module("render", os.path.join(HERE, "render.py"))
    forms = render_mod.project_version(os.path.join(ROOT, "CMakeLists.txt"))
    check_flatpak(render_mod)
    check_metainfo()
    check_snap()
    check_file_types(forms)
    with tempfile.TemporaryDirectory() as tmp:
        out = a.rendered
        if not out:
            out = tmp
            r = subprocess.run([sys.executable, os.path.join(HERE, "render.py"), "--out", out, "--fake-sums"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                bad("render.py --fake-sums failed:\n" + r.stdout + r.stderr)
        check_rendered(out, forms)
        if a.schemas:
            check_schemas(a.schemas, out, forms)
    for n in notes:
        print(f"note: {n}")
    for p in problems:
        print(f"packaging: {p}", file=sys.stderr)
    print(f"packaging: {'OK' if not problems else str(len(problems)) + ' problem(s)'} (Gridline {forms['full']})")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
