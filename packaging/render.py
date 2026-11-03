#!/usr/bin/env python3
"""K-1: writes the files each distribution channel needs for one Gridline release.

    python3 packaging/render.py --out <dir> [--version <v>] [checksums] [options]

The templates (*.in) in packaging/ carry placeholders; this fills them in from the release:

    @VERSION@        full version, 0.7.2 / 0.7.2-beta.1 (CMakeLists.txt unless --version)
    @NUMERIC@        0.7.2
    @PKGVER@         Arch pkgver (no dash): 0.7.2 / 0.7.2beta.1 (sorts before 0.7.2)
    @RPMRELEASE@     1 / 0.1.beta.1
    @DATE@           release date, YYYY-MM-DD           @RPMDATE@  the same as "Mon Nov 02 2026"
    @MAINTAINER@     FreeBSD port maintainer (--maintainer)
    @SHA256:<name>@  SHA-256 of Gridline-<version>-<name>  (@SHA256UPPER:...@ in capitals)

Checksums come from one of:
    --dist <dir>        the release files themselves (SHA-256 and size are computed)
    --sums <file>       a Gridline-<version>-SHA256SUMS file (sizes: --dist or --from-github)
    --from-github       the published release on GitHub: its SHA256SUMS, file sizes, date and commit
    --fake-sums         made-up sums, one per file (CI checks that every template renders; never submit these)

Output (each folder is what goes into that channel's repository or submission):
    flathub/            com.lolitemaultes.Gridline.yml building the release tag (needs --commit)
    aur/gridline-bin/   PKGBUILD + .SRCINFO (the release's Linux tarball)
    aur/gridline/       PKGBUILD + .SRCINFO (built from the source archive)
    homebrew/Casks/     gridline.rb
    winget/manifests/g/Gridline/Gridline/<version>/   the three manifests
    scoop/              gridline.json
    chocolatey/gridline/   gridline.nuspec + tools/
    freebsd/audio/gridline/   Makefile, distinfo, pkg-descr, pkg-plist
    copr/               gridline.spec
    snap/               snapcraft.yaml (copy it to snap/snapcraft.yaml at the source root)
Nothing is uploaded or submitted anywhere.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = "lolitemaultes/Gridline"
APP_ID = "com.lolitemaultes.Gridline"
DEFAULT_MAINTAINER = "188165912+lolitemaultes@users.noreply.github.com"

# Every release file a template names (Gridline-<version>-<name>).
RELEASE_FILES = [
    "source.tar.gz",
    "linux-x86_64.tar.gz", "linux-aarch64.tar.gz",
    "macos-universal.dmg",
    "windows-x64-setup.exe", "windows-arm64-setup.exe",
    "windows-x64.zip", "windows-arm64.zip",
]

# template (relative to packaging/) -> output (relative to --out); {v} is the full version.
TEMPLATES = [
    ("aur/gridline-bin/PKGBUILD.in", "aur/gridline-bin/PKGBUILD"),
    ("aur/gridline-bin/.SRCINFO.in", "aur/gridline-bin/.SRCINFO"),
    ("aur/gridline/PKGBUILD.in", "aur/gridline/PKGBUILD"),
    ("aur/gridline/.SRCINFO.in", "aur/gridline/.SRCINFO"),
    ("homebrew/gridline.rb.in", "homebrew/Casks/gridline.rb"),
    ("winget/Gridline.Gridline.yaml.in", "winget/manifests/g/Gridline/Gridline/{v}/Gridline.Gridline.yaml"),
    ("winget/Gridline.Gridline.installer.yaml.in", "winget/manifests/g/Gridline/Gridline/{v}/Gridline.Gridline.installer.yaml"),
    ("winget/Gridline.Gridline.locale.en-US.yaml.in", "winget/manifests/g/Gridline/Gridline/{v}/Gridline.Gridline.locale.en-US.yaml"),
    ("scoop/gridline.json.in", "scoop/gridline.json"),
    ("chocolatey/gridline.nuspec.in", "chocolatey/gridline/gridline.nuspec"),
    ("chocolatey/tools/chocolateyinstall.ps1.in", "chocolatey/gridline/tools/chocolateyinstall.ps1"),
    ("freebsd/audio/gridline/Makefile.in", "freebsd/audio/gridline/Makefile"),
    ("copr/gridline.spec.in", "copr/gridline.spec"),
]
COPIES = [
    ("chocolatey/tools/chocolateyuninstall.ps1", "chocolatey/gridline/tools/chocolateyuninstall.ps1"),
    ("freebsd/audio/gridline/pkg-descr", "freebsd/audio/gridline/pkg-descr"),
    ("freebsd/audio/gridline/pkg-plist", "freebsd/audio/gridline/pkg-plist"),
    ("snap/snapcraft.yaml", "snap/snapcraft.yaml"),
]
PLACEHOLDER = re.compile(r"@[A-Z0-9_]+(?::[A-Za-z0-9._-]+)?@")


def project_version(cmakelists):
    """The forms of the version at the top of CMakeLists.txt (as tools/release/version.sh)."""
    text = open(cmakelists, encoding="utf-8").read()
    m = re.search(r"^project\(gridline VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text, re.M)
    p = re.search(r'^set\(GRIDLINE_VERSION_PRE "([0-9A-Za-z.]*)"\)', text, re.M)
    if not m or not p:
        raise SystemExit(f"no project(gridline VERSION x.y.z) / GRIDLINE_VERSION_PRE in {cmakelists}")
    return version_forms(m.group(1) + (f"-{p.group(1)}" if p.group(1) else ""))


def version_forms(full):
    m = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?", full)
    if not m:
        raise SystemExit(f"not a Gridline version: {full}")
    numeric, pre = m.group(1), m.group(2) or ""
    return {
        "full": full,
        "numeric": numeric,
        "pre": pre,
        "pkgver": numeric + pre,  # pacman: 0.7.2beta.1 < 0.7.2
        "rpm_release": f"0.1.{pre}" if pre else "1",
    }


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def parse_sums(text, version):
    out = {}
    prefix = f"Gridline-{version}-"
    for line in text.splitlines():
        parts = line.split()
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[0]):
            continue
        name = parts[1].lstrip("*")
        if name.startswith(prefix):
            out[name[len(prefix):]] = parts[0]
    return out


def http_get(url, accept=None):
    req = urllib.request.Request(url, headers={"User-Agent": "Gridline-packaging", **({"Accept": accept} if accept else {})})
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def from_github(version):
    """SHA256SUMS, sizes, date and commit of the published release v<version>."""
    rel = json.loads(http_get(f"https://api.github.com/repos/{REPO}/releases/tags/v{version}"))
    sizes = {}
    sums_url = None
    prefix = f"Gridline-{version}-"
    for a in rel.get("assets", []):
        if a["name"].startswith(prefix):
            sizes[a["name"][len(prefix):]] = a["size"]
        if a["name"] == f"{prefix}SHA256SUMS":
            sums_url = a["browser_download_url"]
    if not sums_url:
        raise SystemExit(f"release v{version} has no {prefix}SHA256SUMS")
    sums = parse_sums(http_get(sums_url).decode("utf-8"), version)
    date = (rel.get("published_at") or "")[:10]
    ref = json.loads(http_get(f"https://api.github.com/repos/{REPO}/git/ref/tags/v{version}"))
    obj = ref["object"]
    if obj["type"] == "tag":  # annotated tag: the commit it points at
        obj = json.loads(http_get(obj["url"]))["object"]
    return sums, sizes, date, obj["sha"]


def local_commit(version):
    try:
        out = subprocess.run(["git", "rev-parse", f"v{version}^{{commit}}"], cwd=ROOT, capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def flathub_manifest(text, version, commit):
    """The in-tree Flatpak manifest with its source switched to the release tag."""
    dir_source = re.compile(r"      # render\.py replaces this.*?\n      - type: dir\n        path: \.\./\.\.\n        skip:\n(?:          - .*\n)+", re.S)
    git_source = (f"      - type: git\n        url: https://github.com/{REPO}.git\n        tag: v{version}\n        commit: '{commit}'\n"
                  f"        x-checker-data:\n          type: git\n          tag-pattern: ^v([0-9]+\\.[0-9]+\\.[0-9]+)$\n")
    text, n = dir_source.subn(git_source, text)
    if n != 1:
        raise SystemExit("packaging/flatpak: the source block of the gridline module was not found")
    cmake = "-DCMAKE_INSTALL_PREFIX=/app"
    if text.count(cmake) != 1:
        raise SystemExit("packaging/flatpak: the cmake configure line was not found")
    # The same build id as the official packages (live collaboration compares it).
    text = text.replace(cmake, f"{cmake} -DGRIDLINE_BUILD_ID={commit[:12]}")
    head = ("# Gridline for Flathub, written by packaging/render.py from packaging/flatpak in the Gridline\n"
            "# source (change it there): the in-tree manifest with the source pinned to the release tag.\n")
    lines = text.split("\n")
    while lines and lines[0].startswith("#"):
        lines.pop(0)
    return head + "\n".join(lines)


def substitute(text, values, sums, missing):
    def repl(m):
        key = m.group(0)[1:-1]
        if key.startswith("SHA256:") or key.startswith("SHA256UPPER:"):
            kind, name = key.split(":", 1)
            if name not in sums:
                missing.add(name)
                return m.group(0)
            return sums[name].upper() if kind == "SHA256UPPER" else sums[name]
        if key in values:
            return values[key]
        return m.group(0)
    return PLACEHOLDER.sub(repl, text)


def render(out, forms, sums, sizes, date, commit, maintainer, timestamp):
    v = forms["full"]
    d = datetime.date.fromisoformat(date)
    values = {
        "VERSION": v,
        "NUMERIC": forms["numeric"],
        "PKGVER": forms["pkgver"],
        "RPMRELEASE": forms["rpm_release"],
        "DATE": date,
        "RPMDATE": d.strftime("%a %b %d %Y"),
        "MAINTAINER": maintainer,
    }
    missing = set()
    written = []
    for src, dst in TEMPLATES:
        text = open(os.path.join(HERE, src), encoding="utf-8").read()
        text = substitute(text, values, sums, missing)
        path = os.path.join(out, dst.format(v=v))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        written.append(path)
    for src, dst in COPIES:
        path = os.path.join(out, dst)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        shutil.copyfile(os.path.join(HERE, src), path)
        written.append(path)
    # FreeBSD distinfo: SHA-256 and size of the source archive.
    src_name = f"Gridline-{v}-source.tar.gz"
    distinfo = os.path.join(out, "freebsd/audio/gridline/distinfo")
    if "source.tar.gz" in sums:
        size = sizes.get("source.tar.gz")
        with open(distinfo, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"TIMESTAMP = {timestamp}\nSHA256 ({src_name}) = {sums['source.tar.gz']}\n")
            f.write(f"SIZE ({src_name}) = {size if size is not None else 0}\n")
        if size is None:
            print("render: no size for the source archive (give --dist or --from-github): distinfo SIZE is 0", file=sys.stderr)
        written.append(distinfo)
    # Flathub: the tag's commit pins the source.
    if commit:
        text = open(os.path.join(HERE, "flatpak", f"{APP_ID}.yml"), encoding="utf-8").read()
        path = os.path.join(out, "flathub", f"{APP_ID}.yml")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(flathub_manifest(text, v, commit))
        written.append(path)
    else:
        print("render: no commit for the tag (give --commit or --from-github): the Flathub manifest is not written", file=sys.stderr)
    left = []
    for path in written:
        try:
            text = open(path, encoding="utf-8").read()
        except UnicodeDecodeError:
            continue
        for m in PLACEHOLDER.finditer(text):
            if not m.group(0).startswith("@SHA256"):
                left.append(f"{os.path.relpath(path, out)}: {m.group(0)}")
    return written, missing, left


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", help="full version (default: CMakeLists.txt)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--sums", help="Gridline-<version>-SHA256SUMS")
    g.add_argument("--from-github", action="store_true", help="read the published release")
    g.add_argument("--fake-sums", action="store_true", help="made-up sums, one per file (template checks only)")
    ap.add_argument("--dist", help="folder with the release files (SHA-256 and sizes)")
    ap.add_argument("--commit", help="the commit of tag v<version> (Flathub)")
    ap.add_argument("--date", help="release date YYYY-MM-DD (default: today, or the release's)")
    ap.add_argument("--maintainer", default=DEFAULT_MAINTAINER, help="FreeBSD port maintainer email")
    a = ap.parse_args()

    forms = version_forms(a.version) if a.version else project_version(os.path.join(ROOT, "CMakeLists.txt"))
    v = forms["full"]
    sums, sizes, date, commit = {}, {}, a.date, a.commit
    if a.fake_sums:
        # distinct, because a manifest validator rejects two installers with the same sum
        sums = {n: hashlib.sha256(n.encode()).hexdigest() for n in RELEASE_FILES}
        sizes = {n: 0 for n in RELEASE_FILES}
        commit = commit or "0" * 40
    if a.dist:
        for n in RELEASE_FILES:
            p = os.path.join(a.dist, f"Gridline-{v}-{n}")
            if os.path.isfile(p):
                sums[n] = sha256_file(p)
                sizes[n] = os.path.getsize(p)
    if a.sums:
        listed = parse_sums(open(a.sums, encoding="utf-8").read(), v)
        for n, h in listed.items():
            if n in sums and sums[n] != h:
                raise SystemExit(f"Gridline-{v}-{n}: the file's SHA-256 is not the one in {a.sums}")
            sums[n] = h
    if a.from_github:
        gs, gsz, gdate, gcommit = from_github(v)
        sums.update(gs)
        for n, s in gsz.items():
            sizes.setdefault(n, s)
        date = date or gdate
        commit = commit or gcommit
    if not commit:
        commit = local_commit(v)
    date = date or datetime.date.today().isoformat()
    timestamp = int(datetime.datetime.combine(datetime.date.fromisoformat(date), datetime.time(12, 0),
                                              tzinfo=datetime.timezone.utc).timestamp())

    written, missing, left = render(os.path.abspath(a.out), forms, sums, sizes, date, commit, a.maintainer, timestamp)
    for p in written:
        print(os.path.relpath(p, os.path.abspath(a.out)))
    if left:
        raise SystemExit("render: placeholders left:\n  " + "\n  ".join(left))
    if missing:
        raise SystemExit("render: no checksum for: " + ", ".join(f"Gridline-{v}-{n}" for n in sorted(missing)))


if __name__ == "__main__":
    main()
