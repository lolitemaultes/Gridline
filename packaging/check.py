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
import re
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
