# Releasing Gridline

Releases are built and published by `.github/workflows/release.yml` when a tag `v<version>` is
pushed. It builds every download (macOS disk image, Windows setup and zip, Linux AppImage,
tar.gz, .deb and .rpm, FreeBSD, OpenBSD and NetBSD tarballs), checks the versions inside them,
starts the programs on their own systems, writes `Gridline-<version>-SHA256SUMS` and creates the
GitHub release "Gridline <version>" with the CHANGELOG.md section `## <version>` as its notes.
"Run workflow" with `dry_run` (the default) builds and checks everything without publishing.

The `media-tools` job first fetches yt-dlp and builds FFmpeg for Import from URL
(`tools/release/fetch-media-tools.py`, pinned in `tools/release/media-tools.json`); the macOS,
Windows and Linux packages carry them, and `Gridline-<version>-media-tools-<system>.zip` is a
release file of its own that Gridline installs on first use where a package has none
(`docs/import-url.md`). To move to newer versions, change the manifest's versions, URLs and
SHA-256 values.

## Where the version is set

Only at the top of `CMakeLists.txt`:

```cmake
project(gridline VERSION 0.7.0 LANGUAGES C CXX)
set(GRIDLINE_VERSION_PRE "beta.1")
```

`GRIDLINE_VERSION_PRE` is a prerelease label (letters and digits separated by dots: `beta.1`,
`rc.2`), or `""` for a release. Together they give the forms below (`cmake/GridlineVersion.cmake`;
`tools/release/version.sh` gives the same ones to the scripts):

| Form | Beta | Release | Used for |
|---|---|---|---|
| Full | `0.7.0-beta.1` | `0.7.0` | `--version`, the About box and splash, the plugins' version text, the user agent, every file name, the git tag, the release title, the README badge, the CHANGELOG section |
| Numbers | `0.7.0` | `0.7.0` | macOS `CFBundleShortVersionString` and `CFBundleVersion` (app, VST3, CLAP, AU), the AU version number, the Windows binary version numbers (the version strings are the full version), the rpm Version |
| Debian | `0.7.0~beta.1` | `0.7.0` | the .deb version (`~` sorts before the release, so 0.7.0 replaces the beta) |
| rpm Release | `0.1.beta.1` | `1` | the .rpm Release (Fedora's convention for prereleases) |

`tools/ci/check-version.sh` (run by CI and the release) checks that the README badge and a
CHANGELOG section say the full version. In the shields.io badge URL a `-` is written `--`:
`https://img.shields.io/badge/version-0.7.0--beta.1-...`.

## Cutting a beta

1. Set `GRIDLINE_VERSION_PRE` (for example `"beta.1"`; the next one `"beta.2"`).
2. Name the CHANGELOG section `## 0.7.0-beta.1` (all changes since the last release; for
   `beta.2`, rename the section and add to it) and update the README badge
   (`alt="Version 0.7.0-beta.1"`, `.../badge/version-0.7.0--beta.1-...`).
   Add `<release version="0.7.0-beta.1" date="<release day>" type="development">` at the top of
   `<releases>` in `assets/linux/com.lolitemaultes.Gridline.metainfo.xml` (software centres on Linux;
   the release workflow checks it).
3. `bash tools/ci/check-version.sh 0.7.0-beta.1`, then run the workflow with `dry_run` on the
   branch and check the files it uploads.
4. Push the tag `v0.7.0-beta.1`.

With a label, the release is created as a GitHub **pre-release** and never marked "Latest"
(`gh release create --prerelease --latest=false`). The update check of people without "Include
beta versions" reads `/releases/latest`, which keeps answering the newest release, so they are
not offered the beta; people with the box ticked read the release list and are (docs/updates.md).

## Making the release final

1. Set `GRIDLINE_VERSION_PRE ""` (the one-line change); `project(VERSION)` stays.
2. Rename the CHANGELOG section `## 0.7.0-beta.1` to `## 0.7.0` (with several betas, merge their
   sections into that one, without repeated items, so the release notes cover everything since the
   last release), drop the beta line at its top, and set the badge to `Version 0.7.0`,
   `.../badge/version-0.7.0-...`. Each version has exactly one section: a beta's section holds
   everything since the last release, and there is never a separate `## 0.7.0` heading waiting
   below it.
3. Add `<release version="0.7.0" date="<release day>">` at the top of `<releases>` in
   `assets/linux/com.lolitemaultes.Gridline.metainfo.xml` (keep the beta entries below it).
4. Check, dry run and push the tag `v0.7.0`.

Without a label the release is a normal GitHub release marked "Latest". It is offered to
everyone, including people on a 0.7.0 beta (a beta sorts before its release). For the next
version, raise `project(VERSION)` and start again with a label or without one.

## Linux build system, Debian and Ubuntu

The Linux packages (`.deb`, `.rpm`, AppImage, `.tar.gz`, x86_64 and aarch64) are built in a
`debian:11` container (glibc 2.31, GCC 10, the system's SDL2 2.0.14, and Kitware's CMake 3.31,
which `tools/ci/install-deps.sh` adds there because the app tests need CMake 3.19), so they run on
every system with glibc 2.31 or later. The AppImage also carries `libgpg-error`, which linuxdeploy
leaves out but minimal Debian 13 systems do not have. The Import from URL tools (ffmpeg) are built in the same container.
`tools/release/check-glibc.sh` fails the build when anything in the AppImage or the `.tar.gz` needs
a newer glibc, and `tools/release/deb-depends.sh` adds the names Debian 13 and Ubuntu 24.04 gave
renamed libraries (`libasound2 | libasound2t64`) to the `.deb`'s Depends. The CI workflow builds
and tests every change on Debian 11 with the same compiler, so code that GCC 10 cannot build fails
there and not at release time.

The release workflow's **Debian** jobs install the release's `.deb` with apt into fresh
`debian:11`, `debian:12`, `debian:13` and `ubuntu:24.04` containers, for x86_64 and ARM64, and run
`tools/ci/debian-check.sh`: every program and plugin finds its libraries, the CLI smoke test, the
app and the AppImage start hidden under Xvfb and draw. The Linux job itself installs the `.deb` on
its Ubuntu 22.04 runner. Debian 11 is past its long-term support: `tools/ci/debian-archive.sh` points
apt at `archive.debian.org` for what the mirrors no longer serve (its security updates today).

To check a release by hand on a Linux computer with Docker:

    docker run --rm -v "$PWD:/w" -w /w debian:11 bash tools/ci/debian-check.sh \
      dist/Gridline-<version>-linux-amd64.deb dist/Gridline-<version>-linux-x86_64.AppImage <version>

## Chromebooks

The release workflow's **ChromeOS** jobs install the release's `.deb` and AppImage, for x86_64 and
ARM64, into the container images ChromeOS itself installs for its Linux development environment
(Google's image of the newest ChromeOS milestone, and its Debian 11 image from the newest milestone
that still offers one; fetched and checked against their SHA-256 by `tools/ci/crostini_image.py`,
imported into Docker by `tools/ci/crostini-docker.sh`), and run `tools/ci/chromeos-check.sh` there:
the CLI smoke test, the app on Mesa with OpenGL capped at 3.0 and 2.1, the Chromebook defaults of a
new installation. The release is only published when they pass. When Google's newest milestone
cannot be found or fetched, `crostini_image.py` uses the pinned milestone `FALLBACK_MILESTONE`
instead, so a broken new index does not stop a release. There is no separate Chromebook package:
Chromebook users install the Linux `.deb` (docs/chromeos.md).

The **ChromeOS** workflow (`.github/workflows/chromeos.yml`) also builds Gridline and runs every
test inside both images, then installs the `.deb` made there. It takes up to 2.5 hours per job, so
it runs every Monday, on each version tag and on "Run workflow", not on every change.

To check a release by hand on a Linux computer with Docker:

    bash tools/ci/crostini-docker.sh /tmp/crostini            # or: ... /tmp/crostini bullseye
    docker run --rm -v "$PWD:/w" -w /w gridline-crostini bash tools/ci/chromeos-check.sh \
      dist/Gridline-<version>-linux-amd64.deb dist/Gridline-<version>-linux-x86_64.AppImage <version>

## Stores and package repositories

Each release also has `Gridline-<version>-symbols-<target>.zip` (macOS, Windows and Linux): the debug
line tables of `gridline-app`, so a crash code from that release resolves to functions and lines with
`tools/crash_symbolize.py` ([crash-reports.md](crash-reports.md)). The programs themselves stay small.

Each release also has `Gridline-<version>-source.tar.gz` (in `SHA256SUMS` like every file), and
the release workflow uploads the workflow artifact `packaging-<version>`: the Flathub manifest,
AUR, Homebrew, winget, Scoop, Chocolatey, FreeBSD port, COPR and snap files filled in for this
release. Submitting them is a manual step per channel.

## Installer design

The disk image window, the Windows setup wizard art and the setup icon are made by
`tools/make_installer_art.py` (Pillow) from the brand: `tools/release/dmg/` (background at 1x and
2x, `layout.json`) and `tools/windows/art/`. `tools/release/make_dmg.py` writes the disk image
with its window (dmgbuild: Finder view settings without Finder or AppleScript, so it runs on CI);
`make_dmg.py --check <mount point>` checks a mounted image. The Windows installer needs Inno Setup
6.6 or later for its dark wizard and art. After a brand change, run
`python3 tools/make_installer_art.py` (`--preview <dir>` writes mock-ups of every design
direction; `--check` fails when the committed files are out of date).
