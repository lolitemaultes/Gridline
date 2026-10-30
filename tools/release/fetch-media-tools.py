#!/usr/bin/env python3
"""J-192: the programs Import from URL runs, for one release package.

    python3 tools/release/fetch-media-tools.py --target <target> --out <dir> [--zip <file>] [--work <dir>]

<target>: macos-universal, linux-x86_64, linux-aarch64, windows-x64, windows-arm64.

Writes <dir> (the package's "tools" folder):
    yt-dlp[.exe]        yt-dlp's official release binary for the target (public domain)
    yt-dlp.version      its version (Gridline's daily update compares it)
    ffmpeg[.exe], ffprobe[.exe]
                        built here from the official FFmpeg source tarball: audio only, LGPL
                        (no GPL or nonfree parts, no network protocols)
    LICENSES/           yt-dlp's licence, FFmpeg's LGPL text and its other notices
    SOURCES.txt         where everything comes from: URLs, versions, SHA-256, the exact
                        configure line (what the LGPL asks of anyone shipping FFmpeg binaries)
--zip also writes the folder as Gridline's media-tools archive (ffmpeg, ffprobe, LICENSES,
SOURCES.txt; no yt-dlp: Gridline fetches that from yt-dlp's own releases), which Gridline
installs on first use where a package has no tools folder (docs/import-url.md).

Every download is checked against the SHA-256 in tools/release/media-tools.json; nothing is
used that does not match. Runs only in the release workflow (or by hand): never in normal CI,
and nothing it fetches goes into git.

Build hosts: macOS for macos-universal (both architectures, joined with lipo, signed ad hoc),
Linux of the same architecture for linux-*, Linux (x86_64 or aarch64) for windows-* (cross
compiled with the pinned llvm-mingw toolchain). Needs a C compiler, make, tar with xz, and
nasm on x86_64 (else the assembly is left out: slower decoding, same output).
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "media-tools.json")
TARGETS = ["macos-universal", "linux-x86_64", "linux-aarch64", "windows-x64", "windows-arm64"]
MACOS_MIN = "11.0"  # same as the app (release.yml)

# Audio only: every demuxer and parser (any container a site serves), the audio decoders, the
# encoders and muxers Gridline and yt-dlp's fix-ups use. No video encoders, no network, no GPL.
AUDIO_DECODERS = ",".join([
    "aac", "aac_fixed", "aac_latm", "ac3", "ac3_fixed", "eac3", "alac", "als", "ape", "atrac*", "cook", "dca", "dolby_e",
    "dsd_*", "flac", "g723_1", "g729", "gsm", "gsm_ms", "mlp", "mp1", "mp1float", "mp2", "mp2float", "mp3", "mp3float",
    "mp3adu", "mp3adufloat", "mp3on4", "mp3on4float", "nellymoser", "opus", "pcm_*", "adpcm_*", "qdm2", "ra_144",
    "ra_288", "sipr", "tak", "truehd", "tta", "vorbis", "wavpack", "wmalossless", "wmapro", "wmav1", "wmav2",
    "wmavoice", "amrnb", "amrwb", "speex", "qcelp", "evrc", "ilbc", "comfortnoise",
])
ENCODERS = "pcm_f32le,pcm_s16le,pcm_s24le,pcm_s32le,flac,aac,opus,libopus"
MUXERS = "wav,w64,flac,mp4,ipod,mov,matroska,webm,ogg,opus,adts,mp3,mpegts,null"
FILTERS = "aresample,aformat,anull,atrim,asetpts,abuffer,abuffersink,volume,amix,aselect,null,format"


def log(msg):
    print(f"[media-tools] {msg}", flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(url, dest, want):
    """Downloads url to dest (reused when already there and intact) and checks its SHA-256."""
    if os.path.exists(dest) and sha256(dest) == want:
        return dest
    log(f"download {url}")
    tmp = dest + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "Gridline-release"})
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    got = sha256(tmp)
    if got != want:
        os.remove(tmp)
        sys.exit(f"SHA-256 mismatch for {url}: got {got}, the manifest says {want}")
    os.replace(tmp, dest)
    return dest


def run(cmd, cwd=None, env=None):
    log("$ " + " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def untar(path, into):
    os.makedirs(into, exist_ok=True)
    with tarfile.open(path) as t:
        for m in t.getmembers():  # nothing may land outside `into`
            p = os.path.realpath(os.path.join(into, m.name))
            if not p.startswith(os.path.realpath(into) + os.sep) and p != os.path.realpath(into):
                sys.exit(f"unsafe path in {path}: {m.name}")
        t.extractall(into)


def configure_flags(extra):
    return [
        "--disable-everything", "--disable-autodetect", "--disable-network", "--disable-doc", "--disable-htmlpages",
        "--disable-manpages", "--disable-podpages", "--disable-txtpages", "--disable-ffplay", "--disable-debug",
        "--disable-shared", "--enable-static", "--enable-ffmpeg", "--enable-ffprobe", "--enable-swresample",
        "--disable-avdevice", "--disable-swscale", "--enable-small",
        "--enable-demuxers", "--enable-parsers", "--enable-bsfs",
        f"--enable-decoder={AUDIO_DECODERS}", f"--enable-encoder={ENCODERS.replace(',libopus', '')}",
        f"--enable-muxer={MUXERS}", f"--enable-filter={FILTERS}", "--enable-protocol=file,pipe",
    ] + extra


def build_ffmpeg(src, build, prefix, extra, env=None, jobs=None):
    os.makedirs(build, exist_ok=True)
    flags = configure_flags(extra)
    run([os.path.join(src, "configure"), f"--prefix={prefix}"] + flags, cwd=build, env=env)
    run(["make", f"-j{jobs or os.cpu_count() or 4}"], cwd=build, env=env)
    run(["make", "install"], cwd=build, env=env)
    return flags


def x86_asm(extra, arch):
    if arch in ("x86_64", "x64") and not shutil.which("nasm"):
        log("nasm is missing: building without x86 assembly")
        extra.append("--disable-x86asm")
    return extra


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, choices=TARGETS)
    ap.add_argument("--out", required=True)
    ap.add_argument("--zip")
    ap.add_argument("--work", default=None)
    ap.add_argument("--jobs", type=int, default=None)
    a = ap.parse_args()
    m = json.load(open(MANIFEST))
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or os.path.join(out + ".work"))
    os.makedirs(work, exist_ok=True)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "LICENSES"))
    windows = a.target.startswith("windows")
    exe = ".exe" if windows else ""

    # yt-dlp: the official binary.
    y = m["yt-dlp"]
    asset = y["assets"][a.target]
    yp = fetch(y["url"].format(asset=asset["name"]), os.path.join(work, asset["name"]), asset["sha256"])
    shutil.copy2(yp, os.path.join(out, "yt-dlp" + exe))
    os.chmod(os.path.join(out, "yt-dlp" + exe), 0o755)
    with open(os.path.join(out, "yt-dlp.version"), "w") as f:
        f.write(y["version"] + "\n")
    shutil.copy2(os.path.join(HERE, "media-tools", "yt-dlp-LICENSE.txt"), os.path.join(out, "LICENSES", "yt-dlp-LICENSE.txt"))

    # FFmpeg: from the official source.
    ff = m["ffmpeg"]
    tarball = fetch(ff["url"], os.path.join(work, os.path.basename(ff["url"])), ff["sha256"])
    srcroot = os.path.join(work, "src")
    if os.path.exists(srcroot):
        shutil.rmtree(srcroot)
    untar(tarball, srcroot)
    src = os.path.join(srcroot, f"ffmpeg-{ff['version']}")
    host = platform.machine().lower()
    configures = []
    if a.target == "macos-universal":
        if sys.platform != "darwin":
            sys.exit("macos-universal is built on macOS")
        parts = []
        for arch in ("arm64", "x86_64"):
            prefix = os.path.join(work, f"install-{arch}")
            cflags = f"-arch {arch} -mmacosx-version-min={MACOS_MIN}"
            extra = [f"--arch={arch}", "--target-os=darwin", "--enable-cross-compile", "--cc=clang", f"--extra-cflags={cflags}",
                     f"--extra-ldflags={cflags}", "--enable-zlib"]
            if arch == "x86_64":
                extra = x86_asm(extra, arch)
            configures.append(build_ffmpeg(src, os.path.join(work, f"build-{arch}"), prefix, extra, jobs=a.jobs))
            parts.append(prefix)
        for prog in ("ffmpeg", "ffprobe"):
            run(["lipo", "-create", "-output", os.path.join(out, prog)] + [os.path.join(p, "bin", prog) for p in parts])
            run(["codesign", "--force", "--sign", "-", "--timestamp=none", os.path.join(out, prog)])
        run(["codesign", "--force", "--sign", "-", "--timestamp=none", os.path.join(out, "yt-dlp")])
    elif a.target.startswith("linux"):
        arch = a.target.split("-", 1)[1]
        if not sys.platform.startswith("linux") or host not in (arch, "amd64" if arch == "x86_64" else arch, "arm64" if arch == "aarch64" else arch):
            sys.exit(f"{a.target} is built on Linux {arch}")
        prefix = os.path.join(work, "install")
        extra = x86_asm(["--enable-zlib", "--extra-ldflags=-static-libgcc"], arch)
        configures.append(build_ffmpeg(src, os.path.join(work, "build"), prefix, extra, jobs=a.jobs))
        for prog in ("ffmpeg", "ffprobe"):
            shutil.copy2(os.path.join(prefix, "bin", prog), os.path.join(out, prog))
            run(["strip", os.path.join(out, prog)])
    else:  # windows-*: cross compiled on Linux with llvm-mingw
        if not sys.platform.startswith("linux"):
            sys.exit("windows-* is cross compiled on Linux")
        lm = m["llvm-mingw"]["hosts"]["aarch64" if host in ("aarch64", "arm64") else "x86_64"]
        lt = fetch(lm["url"], os.path.join(work, os.path.basename(lm["url"])), lm["sha256"])
        tc = os.path.join(work, "llvm-mingw")
        if not os.path.isdir(tc):
            untar(lt, tc)
        tcbin = os.path.join(tc, os.listdir(tc)[0], "bin")
        env = dict(os.environ, PATH=tcbin + os.pathsep + os.environ["PATH"])
        arch = "aarch64" if a.target == "windows-arm64" else "x86_64"
        triple = f"{arch}-w64-mingw32"
        prefix = os.path.join(work, "install")
        extra = [f"--arch={arch}", "--target-os=mingw32", "--enable-cross-compile", f"--cross-prefix={triple}-", f"--cc={triple}-clang",
                 f"--cxx={triple}-clang++", "--extra-ldflags=-static"]
        extra = x86_asm(extra, arch)
        configures.append(build_ffmpeg(src, os.path.join(work, "build"), prefix, extra, env=env, jobs=a.jobs))
        for prog in ("ffmpeg", "ffprobe"):
            shutil.copy2(os.path.join(prefix, "bin", prog + ".exe"), os.path.join(out, prog + ".exe"))
            run([os.path.join(tcbin, f"{triple}-strip"), os.path.join(out, prog + ".exe")])
    for f in ("COPYING.LGPLv2.1", "LICENSE.md", "CREDITS"):
        if os.path.exists(os.path.join(src, f)):
            shutil.copy2(os.path.join(src, f), os.path.join(out, "LICENSES", "ffmpeg-" + f + ("" if f.endswith(".md") else ".txt")))
    for prog in ("ffmpeg", "ffprobe"):
        p = os.path.join(out, prog + exe)
        os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    with open(os.path.join(out, "SOURCES.txt"), "w") as f:
        f.write("Programs Gridline runs for File > Import > Import from URL (docs/import-url.md)\n\n")
        f.write(f"yt-dlp {y['version']} ({y['license']})\n  {y['source']}\n")
        f.write(f"  binary: {y['url'].format(asset=asset['name'])}\n  sha256: {asset['sha256']}\n")
        f.write("  The yt-dlp program carries Python and libraries under their own licences; see its source.\n\n")
        f.write(f"FFmpeg {ff['version']} ({ff['license']})\n")
        f.write(f"  Built by Gridline's release workflow (tools/release/fetch-media-tools.py) from the unmodified source:\n")
        f.write(f"  {ff['url']}\n  sha256: {ff['sha256']}\n")
        f.write("  The same source is available from https://ffmpeg.org/download.html; the licence text is in LICENSES/.\n")
        for c in configures:
            f.write("  configure " + " ".join(c) + "\n")
        f.write("\nFFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project. Gridline is not affiliated with\n"
                "the FFmpeg or yt-dlp projects.\n")

    # Sanity: the programs answer their version (where they can run here).
    native = (a.target == "macos-universal" and sys.platform == "darwin") or (a.target.startswith("linux") and sys.platform.startswith("linux"))
    if native:
        run([os.path.join(out, "ffmpeg"), "-hide_banner", "-version"])
        run([os.path.join(out, "yt-dlp"), "--version"])

    if a.zip:
        z = os.path.abspath(a.zip)
        if os.path.exists(z):
            os.remove(z)
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            for name in ("ffmpeg" + exe, "ffprobe" + exe, "SOURCES.txt"):
                zf.write(os.path.join(out, name), name)
            for lic in sorted(os.listdir(os.path.join(out, "LICENSES"))):
                if lic.startswith("ffmpeg-"):
                    zf.write(os.path.join(out, "LICENSES", lic), "LICENSES/" + lic)
        log(f"wrote {z}")
    log(f"wrote {out}")


if __name__ == "__main__":
    main()
