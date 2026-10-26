# Releasing Gridline

Releases are built and published by `.github/workflows/release.yml` when a tag `v<version>` is
pushed. It builds every download (macOS disk image, Windows setup and zip, Linux AppImage,
tar.gz, .deb and .rpm, FreeBSD, OpenBSD and NetBSD tarballs), checks the versions inside them,
starts the programs on their own systems, writes `Gridline-<version>-SHA256SUMS` and creates the
GitHub release "Gridline <version>" with the CHANGELOG.md section `## <version>` as its notes.
"Run workflow" with `dry_run` (the default) builds and checks everything without publishing.

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
   `<releases>` in `assets/linux/org.gridline.Gridline.metainfo.xml` (software centres on Linux;
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
   `assets/linux/org.gridline.Gridline.metainfo.xml` (keep the beta entries below it).
4. Check, dry run and push the tag `v0.7.0`.

Without a label the release is a normal GitHub release marked "Latest". It is offered to
everyone, including people on a 0.7.0 beta (a beta sorts before its release). For the next
version, raise `project(VERSION)` and start again with a label or without one.

## Installer design

The disk image window, the Windows setup wizard art and the setup icon are made by
`tools/make_installer_art.py` (Pillow) from the brand: `tools/release/dmg/` (background at 1x and
2x, `layout.json`) and `tools/windows/art/`. `tools/release/make_dmg.py` writes the disk image
with its window (dmgbuild: Finder view settings without Finder or AppleScript, so it runs on CI);
`make_dmg.py --check <mount point>` checks a mounted image. The Windows installer needs Inno Setup
6.6 or later for its dark wizard and art. After a brand change, run
`python3 tools/make_installer_art.py` (`--preview <dir>` writes mock-ups of every design
direction; `--check` fails when the committed files are out of date).
