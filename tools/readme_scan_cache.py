#!/usr/bin/env python3
"""Join scanner results (`gridline --scan-plugin <format> <entry> --out <file>`) into a plugin
cache (plugin-cache.json) for the README's plugin manager screenshot.

Usage: python3 tools/readme_scan_cache.py SCAN_DIR OUT_CACHE.json
Used by tools/readme_shots.sh; the cache layout is src/host/PluginScan.cpp's (version 3).
"""
import json
import os
import sys


def main():
    scan_dir, out = sys.argv[1], sys.argv[2]
    bundles = []
    for name in sorted(os.listdir(scan_dir)):
        if name.endswith(".json"):
            with open(os.path.join(scan_dir, name)) as f:
                r = json.load(f)
            if r.get("ok"):
                bundles.append(r)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump({"version": 3, "bundles": bundles}, f, indent=1)


if __name__ == "__main__":
    main()
