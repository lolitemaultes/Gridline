# Update check

Gridline can tell you when a newer version is published on its GitHub releases page
(<https://github.com/lolitemaultes/Gridline/releases>). It never installs anything by itself.

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
| Debian, Ubuntu and derivatives | `Gridline-<version>-linux-amd64.deb` / `-linux-arm64.deb` |
| Fedora, RHEL, openSUSE and derivatives | `Gridline-<version>-linux-x86_64.rpm` / `-linux-aarch64.rpm` |
| Other Linux x86_64 / aarch64 | `Gridline-<version>-linux-x86_64.AppImage` / `-linux-aarch64.AppImage` (marked executable) |
| FreeBSD, OpenBSD, NetBSD | `Gridline-<version>-<system>-<arch>.tar.gz` |

On Linux the distribution comes from `/etc/os-release` (`ID` and `ID_LIKE`). When a release has no
package for your distribution and processor, the AppImage is offered.

A download whose SHA-256 checksum does not match the one in `SHA256SUMS` is deleted and reported.
If a release has no `SHA256SUMS` file, only the release page is offered.

## Settings

Settings > General > Updates:

* **Check for updates automatically** (on by default): the startup check.
* **Include beta versions** (off by default): also offer prereleases (versions such as
  `0.7.0-beta.1`). Drafts are never offered. Betas are published as GitHub pre-releases, which
  `/releases/latest` never answers, so they reach only people with this box ticked
  (docs/releasing.md).
* **Last checked**: the time of the last successful check, with **Check now**.

## Turning it off

* The setting "Check for updates automatically" stops the startup check; Help > Check for
  updates... still works.
* The command line option `--no-update-check`, the environment variable
  `GRIDLINE_NO_UPDATE_CHECK` (any value) or `CI` (set by build servers) turn checking off
  completely: no request is made, and the manual check says that checks are turned off.

Automated runs (`--hidden`, `--screenshot`, `--profile-frames`, scripted input) and the test suite
never make a network request. `--fake-update <version>` shows the popup for a made-up release
without using the network (for development and screenshots; its "Download installer" writes a small
test file into the settings folder's `dev-downloads`).

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
* What is stored, in `settings.json`: `update.auto`, `update.beta`, `update.lastCheck`,
  `update.skipVersion` and `update.remindAfter`.
