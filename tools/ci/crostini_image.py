#!/usr/bin/env python3
"""Downloads the container image a Chromebook's Linux development environment installs.

ChromeOS installs its Linux container from Google's image server (a simplestreams index per
ChromeOS milestone at storage.googleapis.com/cros-containers/<milestone>/). This fetches the newest
milestone's default Debian image for one architecture (or the image of an older Debian release
that Chromebooks set up before, such as bullseye), checks its SHA-256 against the index and writes
the root file system (squashfs). The CI imports it as a Docker image, so the packages are tested on
the same system a Chromebook runs, with Google's own guest packages and sound settings.

  python3 tools/ci/crostini_image.py --arch amd64|arm64 --out rootfs.squashfs [--milestone N] [--release R]

When the newest milestone cannot be found or its image cannot be fetched, the pinned milestone
FALLBACK_MILESTONE is used instead, so a broken new index at Google does not stop a release.

Prints "milestone=<n> release=<debian codename> version=<image date>" on success.
"""
import argparse
import hashlib
import json
import sys
import urllib.error
import urllib.request

BASE = "https://storage.googleapis.com/cros-containers"
# Debian releases in order; the newest one an index offers as "default" is used.
RELEASES = ["buster", "bullseye", "bookworm", "trixie", "forky", "duke"]
# A milestone whose index and images were checked to work (2026-11-08: bookworm and bullseye, both
# architectures). Move it forward now and then; Google keeps old milestones' images.
FALLBACK_MILESTONE = 150


def fetch_json(url):
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return json.load(r)
    except (urllib.error.HTTPError, urllib.error.URLError, ValueError):
        return None


def newest_milestone(start, misses_allowed=4):
    """The highest milestone with an index, counting up from `start` until a run of misses."""
    best, m, misses = None, start, 0
    while misses < misses_allowed:
        if fetch_json(f"{BASE}/{m}/streams/v1/index.json"):
            best, misses = m, 0
        else:
            misses += 1
        m += 1
    return best


def pick(images, arch, release=None):
    """(release, version, item) of the newest default image for `arch` (of `release` if given)."""
    found = []
    for name, product in images.get("products", {}).items():
        parts = name.split(":")  # debian:<release>:<arch>:default
        if len(parts) != 4 or parts[0] != "debian" or parts[2] != arch or parts[3] != "default":
            continue
        if parts[1] not in RELEASES or not product.get("versions"):
            continue
        if release and parts[1] != release:
            continue
        version = sorted(product["versions"])[-1]
        item = product["versions"][version]["items"].get("rootfs.squashfs")
        if item:
            found.append((RELEASES.index(parts[1]), parts[1], version, item))
    if not found:
        return None
    found.sort()
    _, release, version, item = found[-1]
    return release, version, item


def fetch_image(milestone, arch, release, out):
    """Downloads and checks one image; returns (release, version) or an error message."""
    images = fetch_json(f"{BASE}/{milestone}/streams/v1/images.json")
    if not images:
        return f"milestone {milestone}: no images.json"
    chosen = pick(images, arch, release)
    if not chosen:
        return f"milestone {milestone}: no default Debian {release or ''} image for {arch}"
    rel, version, item = chosen
    url = f"{BASE}/{milestone}/{item['path']}"
    want = item.get("sha256", "")
    h = hashlib.sha256()
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(out, "wb") as f:
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                h.update(block)
                f.write(block)
    except (urllib.error.URLError, OSError) as e:
        return f"{url}: {e}"
    if not want or h.hexdigest() != want:
        return f"{url}: SHA-256 {h.hexdigest()} does not match the index ({want or 'none given'})"
    return rel, version


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--arch", required=True, choices=["amd64", "arm64"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--milestone", type=int, default=0, help="a ChromeOS milestone (default: the newest)")
    ap.add_argument("--release", choices=RELEASES, help="a Debian release (default: the newest the milestone offers)")
    ap.add_argument("--search-from", type=int, default=140, help="where the search for the newest milestone starts")
    ap.add_argument("--no-fallback", action="store_true", help=f"fail instead of using milestone {FALLBACK_MILESTONE}")
    a = ap.parse_args()

    milestone = a.milestone or newest_milestone(a.search_from)
    result = fetch_image(milestone, a.arch, a.release, a.out) if milestone else f"no image index found at {BASE} from milestone {a.search_from}"
    if a.release and not a.milestone and milestone:
        # Newer milestones stop offering an older release: the newest one that still does.
        m = milestone - 1
        while isinstance(result, str) and "no default Debian" in result and m >= a.search_from:
            milestone, result = m, fetch_image(m, a.arch, a.release, a.out)
            m -= 1
    if isinstance(result, str) and not a.milestone and not a.no_fallback and milestone != FALLBACK_MILESTONE:
        print(f"crostini_image: {result}; using the pinned milestone {FALLBACK_MILESTONE}", file=sys.stderr)
        milestone = FALLBACK_MILESTONE
        result = fetch_image(milestone, a.arch, a.release, a.out)
    if isinstance(result, str):
        sys.exit(result)
    release, version = result
    print(f"milestone={milestone} release={release} version={version}")


if __name__ == "__main__":
    main()
