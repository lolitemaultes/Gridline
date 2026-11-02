# Packaging and distribution channels

Besides the downloads on the GitHub releases page, Gridline is prepared for these channels. The
files for each live in `packaging/`; the release workflow fills them in for every release, and
the steps to submit them are below. Nothing is ever submitted automatically.

| Channel | Systems | Package | Files | State |
|---|---|---|---|---|
| Flathub | Linux x86_64, aarch64 | `com.lolitemaultes.Gridline` (built from source on Flathub) | `packaging/flatpak/` | ready to submit |
| AUR | Arch Linux and derivatives | `gridline-bin` (release tarball), `gridline` (built from source) | `packaging/aur/` | ready to submit |
| winget | Windows x64, ARM64 | `Gridline.Gridline` (the setup program) | `packaging/winget/` | ready to submit |
| Scoop | Windows x64, ARM64 | `gridline` (the zip) | `packaging/scoop/` | ready (own bucket) |
| Homebrew | macOS | cask `gridline` (the disk image) | `packaging/homebrew/` | ready (own tap) |
| Chocolatey | Windows x64 | `gridline` (the setup program) | `packaging/chocolatey/` | ready to submit |
| FreeBSD ports | FreeBSD | `audio/gridline` (built from source) | `packaging/freebsd/` | ready to submit |
| Fedora COPR | Fedora | `gridline` (built from source) | `packaging/copr/` | ready (own project) |
| Snap Store | Linux | `gridline` | `packaging/snap/` | prepared, see below |

The `.deb`, `.rpm`, AppImage and tarballs on the releases page stay the main Linux downloads.

## What each release produces

* **Release files.** Besides the packages, every release has `Gridline-<version>-source.tar.gz`
  (`git archive` of the tag, with the commit written into `cmake/build-id.txt`, so builds made
  from it carry the same build id as the official packages and can collaborate with them) and
  `Gridline-<version>-SHA256SUMS`, which covers every file.
* **The filled-in channel files.** The release workflow's last job writes them with this
  release's version, date, commit and checksums into the workflow artifact
  `packaging-<version>` (on the release workflow's run page, under Artifacts). Its folders:

      flathub/com.lolitemaultes.Gridline.yml
      aur/gridline-bin/{PKGBUILD,.SRCINFO}   aur/gridline/{PKGBUILD,.SRCINFO}
      homebrew/Casks/gridline.rb
      winget/manifests/g/Gridline/Gridline/<version>/*.yaml
      scoop/gridline.json
      chocolatey/gridline/{gridline.nuspec,tools/}
      freebsd/audio/gridline/{Makefile,distinfo,pkg-descr,pkg-plist}
      copr/gridline.spec
      snap/snapcraft.yaml

* **The same by hand**, for a published release, from the source tree:

      python3 packaging/render.py --out /tmp/gl-packaging --from-github --version 0.7.2

  `--from-github` reads the release's `SHA256SUMS`, file sizes, date and tag commit; `--dist
  <folder>` computes them from local files instead. `python3 packaging/check.py` checks the
  templates (also a test: `ctest -R packaging_manifests`).

The **Packaging** workflow (`.github/workflows/packaging.yml`) checks the files on every change:
templates render, every manifest validates against its published schema, the AppStream metadata
and the desktop entry validate. On "Run workflow", a version tag and every Monday it also builds
the Flatpak (and runs Flathub's linter on it), the AUR source package (makepkg and namcap), the
COPR spec (rpmbuild), the FreeBSD port (portlint, stage, check-plist) and the snap.

## Application id

Gridline's id is `com.lolitemaultes.Gridline`: the AppStream id (in
`assets/linux/com.lolitemaultes.Gridline.metainfo.xml`) and the Flatpak id. It comes from the
domain `lolitemaultes.com`, which Flathub verifies (see step 5 under Flathub). Until 0.7.1 the
AppStream id was `org.gridline.Gridline`; the metadata lists it under `<replaces>` so software
centres treat the new id as the same application. The desktop entry and icon keep the name
`gridline` in the `.deb`, `.rpm`, AppImage and tarballs; inside the Flatpak they are renamed to
the id, as Flathub requires. The macOS bundle identifier stays `org.gridline.Gridline`.

## Flathub

**What the Flatpak contains.** Gridline built from source on the freedesktop runtime 26.08, with
Import from URL's programs built from their official sources too: FFmpeg audio only, LGPL (the
same configure options as the release packages), and yt-dlp from its source archive, run by the
runtime's Python. Flathub builds without network access, so every source is pinned with its
SHA-256 in the manifest.

**Differences from the other Linux packages** (worth knowing for support questions):

* Settings, the stem models and installed media tools live in
  `~/.var/app/com.lolitemaultes.Gridline/` (config, data) instead of `~/.config/gridline`
  and `~/.local/share/gridline`. To bring settings over:
  `cp -r ~/.config/gridline ~/.var/app/com.lolitemaultes.Gridline/config/`.
* Plugins: Gridline searches `~/.vst3`, `~/.clap`, `~/.lv2` and `~/.vst` as usual, plus the
  plugins packaged for Flatpak (`org.freedesktop.LinuxAudio.Plugins.*`, installed with
  `flatpak install flathub org.freedesktop.LinuxAudio.Plugins.<name>`). Plugins installed by the
  system's package manager in `/usr/lib/vst3` and friends are outside the sandbox, and many would
  not load against the runtime's libraries anyway; the same holds for every DAW on Flathub.
* The Gridline Suite (Gridline's instruments and effects as plugins for other hosts) is not part
  of the Flatpak: copies made from inside the sandbox would depend on the runtime's libraries,
  which other hosts do not have. Hosts get it from the `.deb`, `.rpm`, AppImage or tarball.
* Updates come from Flathub. The update check still tells people about a new release (it opens the
  release page and says to update from the software store); it never offers the setup download.
* yt-dlp keeps itself current as in the other packages (a newer copy goes into Gridline's own data
  folder in `~/.var/app/...`); the copy in the Flatpak is the one from the manifest.

**Permissions and why** (Flathub's reviewers ask; this is the text for the submission):

| Permission | Why |
|---|---|
| `--socket=x11`, `--share=ipc` | The window. Plugin editors are X11 windows embedded in Gridline's, so X11 (XWayland on Wayland desktops) is required, not a fallback. |
| `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0` | Playback and recording (PulseAudio or PipeWire), JACK clients through PipeWire. |
| `--device=all` | MIDI keyboards and controllers (the ALSA sequencer and raw MIDI devices) and the GPU. |
| `--share=network` | Live collaboration between Gridline users, the update check, Import from URL, downloading the stem separation models. |
| `--filesystem=home` | Projects, samples and the plugin folders (`~/.vst3`, `~/.clap`, `~/.lv2`, `~/.vst`) anywhere in the home folder. Gridline's browser reads whole folders (samples, presets, projects), which the file chooser portal cannot provide. |
| `--env=SDL_VIDEO_*_WMCLASS` | The window class is the app id, so docks show the right icon. |
| `--env=VST3_PATH=...` (and CLAP, LV2, VST) | The plugin extension folders. |

**Submission checklist** (once per application; Flathub's docs:
<https://docs.flathub.org/docs/for-app-authors/submission>):

1. Run the Packaging workflow ("Run workflow") on the release commit and check that the Flatpak
   jobs pass, including Flathub's linter. Install the bundle it uploads
   (`flatpak install --user Gridline-x86_64.flatpak`) and try audio, MIDI, a plugin, Import from
   URL and a collaborative session.
2. Take the release's `packaging-<version>/flathub/com.lolitemaultes.Gridline.yml`.
3. Fork <https://github.com/flathub/flathub>, create a branch from `new-pr`, add the manifest at
   the top level of the branch, and open a pull request against `new-pr` titled
   `Add com.lolitemaultes.Gridline`. In the description, paste the permissions table above
   and say that the screenshots, metadata and icons come from the upstream source.
4. Answer the reviewers; a build bot builds the pull request (`bot, build` in a comment starts
   another). Once merged, Flathub creates `flathub/com.lolitemaultes.Gridline`, where you get
   write access.
5. Verify the domain so the app shows as verified: log in to <https://flathub.org>, open the app's
   developer settings and start verification. Flathub gives a token; publish it as the only line
   of `https://lolitemaultes.com/.well-known/org.flathub.VerifiedApps.txt` (served over HTTPS,
   plain text), then confirm on Flathub. The site needs nothing else for this. Until verification
   the app is listed as unverified; the id itself is accepted from the first submission.

**Every release after that:** in `flathub/com.lolitemaultes.Gridline`, replace the manifest
with the new `packaging-<version>/flathub/...yml` (or merge the pull request that Flathub's
external data checker opens: it watches the git tag, FFmpeg and yt-dlp). Prereleases go to
Flathub's beta branch only after the app is in stable.

## AUR (Arch User Repository)

Two packages: `gridline-bin` (the release's Linux tarball: quick to install, the same build as the
official downloads, including Import from URL's programs) and `gridline` (built from the source
archive; yt-dlp and ffmpeg as optional dependencies from Arch's repositories).

**Checklist** (AUR rules: <https://wiki.archlinux.org/title/AUR_submission_guidelines>):

1. Once: an account on <https://aur.archlinux.org> with your SSH public key.
2. For each package (`gridline-bin`, `gridline`):

       git clone ssh://aur@aur.archlinux.org/gridline-bin.git
       cp packaging-<version>/aur/gridline-bin/{PKGBUILD,.SRCINFO} gridline-bin/
       cd gridline-bin
       makepkg --printsrcinfo > .SRCINFO   # the same as the one provided; makepkg has the last word
       namcap PKGBUILD && makepkg -si      # builds and installs it on an Arch machine
       git add PKGBUILD .SRCINFO && git commit -m "Update to <version>" && git push

   Cloning a name nobody has used yet creates the package on the first push.
3. Every release: the same with the new files (bump `pkgrel` instead when only the PKGBUILD
   changes). Prereleases are not published to the AUR.

## winget (Windows Package Manager)

**Checklist** (<https://learn.microsoft.com/windows/package-manager/package/repository>):

1. On Windows: `winget validate --manifest packaging-<version>\winget\manifests\g\Gridline\Gridline\<version>`
   and, in a clone of `microsoft/winget-pkgs`, `.\Tools\SandboxTest.ps1 <that folder>`.
2. Fork `microsoft/winget-pkgs`, copy the folder to `manifests/g/Gridline/Gridline/<version>/`,
   commit (`New package: Gridline.Gridline version <version>`) and open a pull request. The bots
   install and scan the setup program; a moderator approves.
3. Every release: the same with the new version folder (`wingetcreate update Gridline.Gridline
   --version <version> --urls <x64 url> <arm64 url> --submit` does it in one step).

The installer asks for administrator rights itself (it installs into Program Files), which the
manifest declares. If the scanners flag the unsigned setup program, the pull request page names the
engine; a false positive is reported to that vendor.

## Scoop

Scoop installs the portable zip. The main and extras buckets list only well known software, so the
practical route is a bucket of Gridline's own:

1. Create a repository `lolitemaultes/scoop-gridline` with `bucket/gridline.json` from
   `packaging-<version>/scoop/gridline.json`.
2. Users: `scoop bucket add gridline https://github.com/lolitemaultes/scoop-gridline` then
   `scoop install gridline`.
3. The manifest has `checkver` and `autoupdate`: running Scoop's `checkver.ps1 -App gridline -Update`
   (or the Excavator action in that repository) updates the version and hashes from the release's
   `SHA256SUMS` by itself. Later, when Gridline is well known, the same file can be proposed to
   `ScoopInstaller/Extras`.

## Homebrew (macOS)

Homebrew's main cask repository only accepts apps that pass macOS Gatekeeper; Gridline's disk
image is signed ad hoc, not notarised, so it goes into a tap of its own:

1. Create a repository `lolitemaultes/homebrew-gridline` with `Casks/gridline.rb` from
   `packaging-<version>/homebrew/Casks/gridline.rb`.
2. Check it: `brew audit --cask --strict lolitemaultes/gridline/gridline` and
   `brew install --cask lolitemaultes/gridline/gridline`.
3. Users: `brew install --cask lolitemaultes/gridline/gridline`. The cask's caveats explain the
   first start (Control-click > Open, as for the disk image).
4. Every release: replace the file (version and `sha256` change). `brew livecheck` finds new
   releases.

## Chocolatey

1. Once: an account on <https://community.chocolatey.org> and its API key
   (`choco apikey --key <key> --source https://push.chocolatey.org/`).
2. `cd packaging-<version>\chocolatey\gridline`, `choco pack`, then try it:
   `choco install gridline --source . -y` and `choco uninstall gridline -y`.
3. `choco push gridline.<version>.nupkg --source https://push.chocolatey.org/`. The first version
   goes through moderation (automatic checks, a virus scan, then a person), which can take days.
4. Every release: the same with the new files. The package covers Windows x64 (Chocolatey has no
   separate ARM64 download field; ARM64 users use winget, Scoop or the setup program).

## FreeBSD ports

The port builds the release's source archive with the system's SDL2 and sndio; Import from URL uses
FreeBSD's `yt-dlp` and `ffmpeg` packages (the IMPORTURL option, on by default).

**Checklist** (<https://docs.freebsd.org/en/books/porters-handbook/>):

1. Set the maintainer to an address you read: `render.py --maintainer you@example.org` (the
   default is the GitHub no-reply address, which FreeBSD cannot use).
2. On FreeBSD with a ports tree: copy `packaging-<version>/freebsd/audio/gridline` to
   `/usr/ports/audio/gridline`, then `portlint -AC`, `make stage check-plist`, `make package`, and
   ideally `poudriere testport audio/gridline` for a clean build.
3. Open a bug at <https://bugs.freebsd.org/bugzilla/> (Product: Ports & Packages, Component:
   Individual Port(s), summary `[NEW PORT] audio/gridline: Pattern-based music workstation`) and
   attach the port as a patch (`git diff` against the ports tree, with the new files added).
4. Every release: a bug with the update (`DISTVERSION`, `distinfo`), or once you are the
   maintainer, the same as a patch for the committer.

## Fedora COPR

COPR builds rpms for every Fedora release from the spec:

1. Once: an account on <https://copr.fedorainfracloud.org> and a project `gridline` (chroots:
   the current Fedora releases, x86_64 and aarch64).
2. Each release: New build > Upload: `packaging-<version>/copr/gridline.spec` (COPR fetches the
   source archive named in it), or `copr-cli build gridline gridline.spec`.
3. Users: `sudo dnf copr enable lolitemaultes/gridline && sudo dnf install gridline`.

The `.rpm` on the releases page keeps working for Fedora, RHEL and openSUSE; COPR adds automatic
updates through dnf.

## Snap Store (prepared)

`packaging/snap/snapcraft.yaml` builds a strictly confined snap (copy it to `snap/snapcraft.yaml`
at the source root and run `snapcraft pack`; the Packaging workflow builds it too). Two things
stand before publishing:

* Plugins: strict snaps cannot read hidden folders in the home folder, which is where plugins live
  (`~/.vst3`, `~/.clap`, `~/.lv2`, `~/.vst`). The snap asks for them through the `personal-files`
  interface, which the Snap Store has to approve: register the name (`snapcraft register
  gridline`), upload, then ask on <https://forum.snapcraft.io> (category store-requests) for
  `personal-files` read access to those four folders, explaining that a music workstation loads
  the user's plugins from them. Without it the snap works, with Gridline's own instruments and
  effects only. Classic confinement would avoid the request but needs a review that is rarely
  granted to applications of this kind.
* Plugins from other packages would also be built against different libraries than the snap's, as
  with the Flatpak.

Until then the snap is not recommended over the Flatpak, AppImage, `.deb` and `.rpm`.

## Not now

* **Debian, Ubuntu, Fedora, openSUSE official repositories.** They need a maintainer (or a
  sponsor) inside each distribution, and their policies ask that the libraries Gridline carries in
  `third_party/` (Dear ImGui, RtMidi, Rubber Band, Lua, libjuice and others) use the system's
  copies instead. That is ongoing work for a packager, not files in this repository. COPR, the
  `.deb` and `.rpm` and the Flatpak cover those systems meanwhile.
* **openSUSE Build Service.** It could build `.deb` and `.rpm` repositories for many distributions
  from the COPR spec; it adds little over COPR plus the release packages for now.
