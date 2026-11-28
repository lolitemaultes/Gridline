# Updates

Gridline can tell you when a newer version is published on its GitHub releases page
(<https://github.com/lolitemaultes/Gridline/releases>), and, where the copy you run can update
itself, install it on the start screen and restart into it.

## Automatic updates on the start screen

With **Check for updates** and **Install updates automatically** both on (the default), the start
screen checks for the newest release made for the way this copy was installed. When there is one,
it shows **Downloading Gridline 0.7.9** with a progress bar and the size, checks the download,
installs it and restarts Gridline in the new version. Your settings and projects are never
changed: the updater replaces the program only.

| Installed as | What happens |
|---|---|
| AppImage | The new AppImage is downloaded next to the old one, test-run, and swapped in place in one step. |
| The `.tar.gz` folder (Linux) | Unpacked next to the folder, test-run, and the whole folder is swapped in one step. |
| macOS app (`Gridline.app` in a folder you can write to) | The disk image is opened in the background, the app is copied out, its code signature, bundle id and minimum macOS version are checked, and the app is swapped in one step. |
| The `.deb` or `.rpm` package | Downloaded and checked, then the system asks for your password at once and its package manager installs it. The start screen says "Installing Gridline 0.7.9: your system will ask for your password". |
| Windows setup | Downloaded and checked, then Gridline closes, Windows asks for permission and the setup runs silently. The start screen says "Installing Gridline 0.7.9: Windows will ask for permission". |
| Flatpak, snap, Homebrew, Scoop, Chocolatey, winget, AUR, COPR, FreeBSD ports, a distribution's own package | Updated by that store or package manager. Gridline only tells you (the popup below) and Settings shows the command, for example `brew upgrade --cask gridline`. |
| macOS app run from the disk image, or from a quarantined copy macOS moved (App Translocation), or in a folder you can't write to | Not updated in place. Move Gridline to Applications first; the popup still tells you about new versions. |
| The `.tar.gz` installed into a prefix with its `install.sh` | Not updated by itself (its files are spread over the prefix, so there is no safe way back). The popup tells you; unpack the new `.tar.gz` and run its `install.sh` again. |
| A development build | Never updated. |

The start screen has no buttons: it shows what is happening and a progress bar, and the two
switches in Settings decide everything:

* **Check for updates** off: no request at all.
* **Check for updates** on, **Install updates automatically** off: only a check; a new version is
  offered in the popup after Gridline has started, never on the start screen.
* Both on: fully automatic, as above.

A check that does not answer within about four seconds, or a download that stalls, lets Gridline
start by itself; the download carries on behind it and is installed at the next start. If you
close the password or permission prompt, Gridline starts as usual and tries again a day later.

### Safety

* Every file comes over HTTPS from the release, is checked against the release's `SHA256SUMS`, and
  its size must match. A file that does not match is deleted and never installed. The file name
  must carry the release's own version, and an older version is never installed.
* When this build carries a release signing key, `SHA256SUMS` must also be signed with it
  (minisign, Ed25519), and the signature must name that release's file: a release without a
  valid signature is refused.
* The new version is test-run (`--version`) before anything is replaced, so a version that cannot
  run on this computer is refused before the swap.
* The swap is atomic, and the old version is kept until the new one has started. A small watcher
  waits for the old Gridline to quit, starts the new one and, if it fails to start, puts the old
  version back and starts it. If the new version fails at its first starts, it puts the old one
  back by itself. Gridline then says once "Gridline 0.7.9 didn't start, so 0.7.8 is back", and does
  not install that version by itself again.
* An update cut short (Gridline killed, the power lost) is finished or undone at the next start.
  Only one Gridline updates at a time. Leftover files are deleted at the next start.

### When something goes wrong

A failure never stops Gridline from starting: the start screen shows "Couldn't update. Starting
Gridline 0.7.8" for a moment and goes on. No network, a server error, a captive portal page or a
broken answer are quiet and tried again at a later start. Other failures (a damaged download, a
full disk, a folder you can't write to, a version that needs a newer system) are shown once after
start-up as "Couldn't update to Gridline 0.7.9" with **Details** (the update log) and **Try
again**. After a failure the next tries wait 1 hour, 4 hours, then a day, then only when you ask.
A cut-off download is resumed where it stopped. Everything is written to `updates/update.log` in
Gridline's settings folder.

## When it checks

* **At startup**, once the main window is up, at most once every 24 hours. The check runs on a
  background thread: the interface and the audio never wait for it. If the computer is offline or
  GitHub does not answer, nothing is shown and the check is tried again at the next start.
* **Help > Check for updates...** and **Settings > General > Updates > Check now** always ask,
  and always report the result, including "You're up to date (0.6.0)" or why the check failed.
* **On the command line**: `gridline --check-updates` (or `gridline-app --check-updates`) prints
  the result and exits with 0 (up to date, no release yet, or checks turned off), 2 (an update is
  available; the download for this computer and a short excerpt of the release notes are printed
  too) or 1 (the check failed). Add `--beta` to include beta versions.

## The popup

When a newer release is found, Gridline shows "Gridline 0.6.1 is available (you have 0.6.0)" with
a short, scrollable excerpt of the release notes (shown as plain text, without images), and these
buttons:

| Button | What it does |
|---|---|
| Download | Opens the release page in your web browser. Works on every system. |
| Download installer | Shown when the release has a download for this computer (see below). Saves it into your Downloads folder with a progress bar, checks it against the release's `SHA256SUMS` file, then shows it in Finder, Explorer or your file manager. The file is not opened or installed, and the running Gridline is not replaced. |
| Remind me later | Closes the popup; the startup check does not offer this release again for 24 hours. Esc does the same. |
| Skip this version | The startup check never offers this version again (a newer one is still offered). Settings > General > Updates shows the skipped version with "Offer it again". |

The popup never interrupts you: while the song plays or records it waits until the transport
stops, and it never appears during the startup splash or on top of another prompt such as "Save
changes to this project?".

Downloads for each system:

| System | File |
|---|---|
| macOS (Apple silicon and Intel) | `Gridline-<version>-macos-universal.dmg` |
| Windows x64 / ARM64 | `Gridline-<version>-windows-x64-setup.exe` / `-windows-arm64-setup.exe` |
| Debian, Ubuntu and derivatives (Zorin OS, Linux Mint, Pop!_OS, elementary OS, Debian on a Chromebook, ...) | `Gridline-<version>-linux-amd64.deb` / `-linux-arm64.deb` |
| Fedora, RHEL, openSUSE and derivatives | `Gridline-<version>-linux-x86_64.rpm` / `-linux-aarch64.rpm` |
| Other Linux x86_64 / aarch64 | `Gridline-<version>-linux-x86_64.AppImage` / `-linux-aarch64.AppImage` (marked executable) |
| FreeBSD, OpenBSD, NetBSD | `Gridline-<version>-<system>-<arch>.tar.gz` |

On Linux the distribution comes from `/etc/os-release` (or `/usr/lib/os-release`), from `ID` and
`ID_LIKE`: any word there that is or contains `debian` or `ubuntu` means the `.deb`. A Gridline
installed from our `.deb` gets the `.deb` even on a system not listed here. When no os-release can be read, the package
database decides (`/var/lib/dpkg`: `.deb`; `/var/lib/rpm`: `.rpm`). The choice is the same whether
Gridline runs from a package, the AppImage or the `.tar.gz`, so on Ubuntu the AppImage also offers
the `.deb`. When a release has no package for your distribution and processor, the AppImage is
offered. Versions before 0.7.0 (its betas included) always download the AppImage on Linux; to
move from one of those to the `.deb`, install the `.deb` from the release page once.

The Flatpak and the snap are updated by their store (Flathub, the Snap Store): there the popup
offers only Download (the release page) and says to update from the software store; it never
downloads a setup program, which could not replace a sandboxed installation.

A download whose SHA-256 checksum does not match the one in `SHA256SUMS` is deleted and reported.
If a release has no `SHA256SUMS` file, only the release page is offered.

## Settings

Settings > General > Updates:

* **Check for updates** (on by default): the startup check.
* **Install updates automatically** (on by default): install on the start screen, as above. Greyed
  out where this copy is updated by its store or package manager. Off: Gridline only offers the
  new version in the popup. Both switches are saved.
* **Installed as**: how Gridline was installed, with the command to update it where a package
  manager does it, the result of the last start-up check, and **Try again** after a failure.
* **Include beta versions** (off by default): also offer prereleases (versions such as
  `0.7.0-beta.1`). Drafts are never offered. Betas are published as GitHub pre-releases, which
  `/releases/latest` never answers, so they reach only people with this box ticked
  (docs/releasing.md).
* **Last checked**: the time of the last successful check, with **Check now**.

## Turning it off

* The setting "Check for updates" stops the startup check and the automatic install; Help > Check for
  updates... still works.
* The command line option `--no-update-check`, the environment variable
  `GRIDLINE_NO_UPDATE_CHECK` (any value) or `CI` (set by build servers) turn checking off
  completely: no request is made, and the manual check says that checks are turned off.

Automated runs (`--hidden`, `--screenshot`, `--profile-frames`, scripted input) and the test suite
never make a network request. `--fake-update <version>` shows the popup for a made-up release
without using the network (for development and screenshots; its "Download installer" writes a small
test file into the settings folder's `dev-downloads`).

## Plugins

The Gridline Suite and Suite Mini plugins look for a new version too, in whatever host they run:

* **When**: at most once a day, when a plugin's editor is open. Every plugin in every host and the
  Gridline app share one small file, `update-check.json` in Gridline's settings folder, so they make
  one request a day between them; the others read the answer. The request runs in the background
  and never holds up the editor or the sound.
* **The notice**: a short line under the plugin's editor, "Version X is out.", with **Download**
  (opens the download page for your system in your browser), **Later** (asks again in a week) and
  **Skip this version**. Nothing is ever downloaded or installed by itself.
* **Turning it off**: untick **Check for updates once a day** in any plugin's menu (it applies to
  every plugin); **Check for updates now** in the same menu checks at once. The environment
  variables `GRIDLINE_NO_UPDATE_CHECK` and `CI` turn plugin checks off as they do for the app.
* **Privacy**: the same single request as the app's (below): the latest release from GitHub, with
  no identifier, settings, host, project or usage information.

## Privacy

A check is a single HTTPS request to the GitHub API:

    GET https://api.github.com/repos/lolitemaultes/Gridline/releases/latest

(`/releases?per_page=20` when beta versions are included). The only headers Gridline adds are
`User-Agent: Gridline/<version>`, `Accept: application/vnd.github+json` and
`X-GitHub-Api-Version: 2022-11-28`. No identifier, account, settings, project information,
system details or usage statistics are sent, and Gridline has no telemetry. Collaborative projects
(docs/collaboration.md) make the same request for their version check, at most once an hour. No cookies are kept.
GitHub sees the request like any visit to its website, including your IP address; see GitHub's
privacy statement. "Download installer" fetches the release's `SHA256SUMS` file and the installer
from GitHub the same way.

Only HTTPS is used, including for redirects, with timeouts (15 seconds for a check; a download
fails after a minute without data). When GitHub limits requests (HTTP 403 or 429), the startup check
stays quiet and the manual check says to try again later.

## How it works

* macOS uses the system's NSURLSession, Windows uses WinHTTP, and Linux and the BSDs run the `curl`
  program (or `wget` when curl is not installed). No extra library is needed. If neither program is
  installed, the check reports that and nothing else happens.
* The running version is compared with the release tag (`v<major>.<minor>.<patch>`) as semantic
  versions, so 0.10.0 is newer than 0.6.0 and 0.7.0-beta.1 is older than 0.7.0.
* What is stored, in `settings.json`: `update.auto`, `update.autoInstall`, `update.beta`,
  `update.lastCheck`, `update.lastResult`, `update.skipVersion` and `update.remindAfter`. The
  automatic install keeps its state in the `updates` folder next to it: `journal.json` (an update
  in progress), `failures.json` (the wait after a failure), `cache.json` (the last answer, sent back
  with its ETag so a repeated check costs nothing) and `update.log`.

## Release signing

Releases are signed by the release workflow when the repository has the secret
`GRIDLINE_UPDATE_SIGNING_KEY` (a minisign secret key without a password). To set it up once:

    minisign -G -W -p gridline-update.pub -s gridline-update.key
    gh secret set GRIDLINE_UPDATE_SIGNING_KEY < gridline-update.key

Put the second line of `gridline-update.pub` into `kReleaseKeys` in `src/core/UpdateKeys.h` and keep
the secret key offline. Builds with a key refuse unsigned releases, and the workflow fails when
there is a key in the source but no secret.
