# Collaborative projects

Two or more people can work on the same Gridline project at the same time, on the same network or
on completely different networks. Every computer keeps the whole project (song and audio files) in
a folder of its own and the computers keep each other up to date directly, peer to peer. There is
no Gridline server and no account.

Each person keeps their own windows, zoom, scroll position and transport. Only the project is
shared: channels, patterns, notes, the playlist, the mixer, automation, plugin settings and the
audio files the project uses.

## Before you start

* **Your profile.** Open Settings > Collaboration and enter a display name. A contact email and a
  profile picture are optional. The profile belongs to you, not to a project.
* **The same Gridline.** Everyone needs the same version (and the same build). If a newer Gridline
  has been released, collaboration asks you to update first (see "Versions" below).
* **Only Gridline's own plugins.** A project that uses a plugin that is not part of Gridline
  (VST3, VST2, CLAP, Audio Unit or LV2, sandboxed or not) cannot be collaborative. The other
  computer might not have the plugin, or have another version of it, and its settings are a sealed
  blob that only that plugin understands, so changes to it could neither be sent as they happen nor
  merged. Gridline lists every such plugin and where it is, so you can replace or remove it.
  In a collaborative project these plugins are hidden from the plugin picker and the browser, and
  anything that would add one (drag and drop, paste, presets, scripts, AI assistants) is refused
  with a message.

## Making a project collaborative

1. Open the project (or start a new one: File > New collaborative project...).
2. Open the Collaboration panel (File > Collaboration..., or Project settings > Collaboration).
3. Under **Share this project**, check the **Name** (it starts with the open project's name; everyone
   you invite sees it) and the **Folder** (by default next to the project file; **Choose...** picks
   another place). The folder is named after the project; the row under it says whether it is new,
   already exists (the project is added to it) or already holds a collaborative project (choose
   another place or name). Press **Make collaborative**. (File > New collaborative project... opens
   the panel with the name field ready to type in.)

Gridline creates the folder and copies the project and every audio file it uses into it. Your
original project file is never changed: it stays where it was, as a normal project of its own, and
nothing you do in the collaborative copy reaches it. From then
on the project saves itself continuously: you never lose more than a fraction of a second of work,
even if the computer crashes. Ctrl+S still works and writes everything immediately.

The folder looks like this:

    My Song/
      My Song.gridline.json     the project, always up to date (opens in any Gridline), named after the project
      Audio/                    every sample, loop and impulse response, named by its content
      .gridline-collab/         the shared history (do not edit)

## Naming and renaming the project

A collaborative project has one name that everyone in it sees: in the window title, the
Collaboration panel, the top bar, invites, Recent collaborative projects and exports. Any member can
change it:

* **Collaboration panel**: click the project's name at the top (a faint box shows around it when the
  pointer is over it). The name becomes a field in the same place; type the new name, then press
  Enter or click anywhere else to rename it for everyone. Escape keeps the old name. To rename this
  computer's project folder as well, use Project info.
* **Project info** (File > Project info...): the **Shared name** row, **Rename...**: the same field
  and options, in the row.
* **Project settings > Collaboration**: the same row.

Names can use any language and symbols, up to 80 characters; spaces at the ends, line breaks and
invisible characters are left out, and a name can't be empty. Renaming to the same name changes
nothing.

* **Collaborators who are online** see the new name at once, with a notice: "Sam renamed the
  project to Night Drive".
* **Collaborators who are offline** get it the next time you are online together (or through a
  third collaborator), like any other change. Their window title, panel and Recent collaborative
  projects follow as soon as they have caught up.
* **If two people renamed it while apart**, the later rename is used for now and both renames go to
  the admin's **Sync review** ("renamed the project from 'Song' to 'Night Drive'"), like other
  changes to the same thing. Two people renaming at the same moment while connected simply end with
  one of the two names (the later one), on every computer.
* **Undo**: a rename is one step of your own undo history. Undo writes the previous name back,
  unless someone else renamed the project after you (then their name stays), like every other undo.
* **Invites** show the name the project had when the invite was made. Whoever joins gets the
  project's current name with the project, never the invite's.

**On disk.** Each computer's project file (`<name>.gridline.json`) is renamed to match the new name,
as soon as the rename arrives. The display name is kept whole; the file name is made safe for
Linux, macOS and Windows (characters like `/ \ : * ? " < > |` become `-`, no dots or spaces at the
start or end, names such as CON or LPT1 get a `_`, long names are shortened). A file of another name
that is already in the folder is never replaced (the project file becomes "Name (2).gridline.json").
A rename that only changes capital letters works on macOS and Windows too. If the file can't be
renamed (Windows: it is open in another program), Gridline keeps the old file name and tries again
later. Gridline finds the project file in its folder whatever it is called (also after you renamed
it by hand), so Recent files and File > Open keep working.

**The folder** keeps its name: you chose it, and other programs or backups may refer to it. While
renaming, **Also rename the project folder on this computer** renames your own folder too
(never anyone else's): Gridline closes the project, renames the folder and opens it again from the
new place, reconnecting in a second or two. If a folder of that name is already there, nothing is
renamed.

## Inviting someone

1. In the Collaboration panel, under **Invite someone**, choose how long the invite should work
   (15 minutes, 1 hour or 24 hours) and press **Create invite**.
2. After a moment the panel shows an invite code that starts with `gridline-invite:`. Send it to
   the person you are inviting by chat, email or as a file (**Save as file...**).

An invite code works **once**, for **one** person, until it expires. Anyone who has the code can use
it, so send it only to the person you mean. When they have joined, both of you see the same
**safety code** (four groups of four digits). Compare it by phone or chat if you want to be sure
nobody else intercepted the invite.

## Joining

1. File > Join collaborative project...
2. Paste the invite code. Gridline shows the project name, who invited you, their fingerprint and
   when the invite expires.
3. Choose where to store the project folder and press **Join**.

Gridline connects to the inviter's computer, receives the project with all its audio files and
opens it. From then on, changes travel both ways as you make them.

### When the inviter's computer can't be reached at once

On the same network, or when the inviter's router allows it, joining needs nothing else. Otherwise
the join window shows a **reply code** after a few seconds. Send it back to the inviter, who pastes
it under the invite in their Collaboration panel (**Use reply**). Both computers then open the
connection towards each other at the same moment, which works through most home routers.

### Joining a project you already have

If you are invited again to a project you already have a copy of (for example because the two of
you could not find each other), Join recognises it: the join window says "You already have this
project on this computer" and Join opens your existing copy and connects with the invite. You stay
the same member, nothing is copied again and no second folder is made.
Joining into a folder that holds a different project is refused.

## Reopening a collaborative project

A collaborative project is an ordinary project folder: close Gridline whenever you like and open
the project again later, in any of these ways:

* **File > Recent collaborative projects** lists every collaborative project on this computer
  (also shown in the Collaboration panel when no project is open). Leaving a project removes it.
* **File > Open collaborative project...** and choose the project's folder (the one that contains
  `.gridline-collab`).
* **File > Open** (or Recent files) and choose the `.gridline.json` file in that folder, or the
  folder itself.
* **Drag** the folder or its `.gridline.json` file onto the Gridline window.

The Collaboration panel opens and Gridline reconnects to the other members by itself, using the
addresses and ports remembered from last time: no code is needed. When the others open the project
too (now or later, in any order), you are connected within a second or two and catch up on each
other's changes. Until then the panel shows each of them as **Offline · last synced ... ·
reconnects when they open the project**.

If someone has the project open and you still are not connected after a minute (a router that
changed its addresses, or a network that blocks the remembered path), the panel says so under
**Connection** and offers **Reconnect with a code...**: both of you choose it for each other, send
each other the codes and paste the other's code. That reconnects without making anyone a new
member. Inviting the person again also works (see above).

## Working together

* Changes appear on the other computers within a fraction of a second. Knob and fader moves stream
  while you drag; the final value always arrives when you let go.
* What a collaborator changed **flashes briefly** in their colour (channel rack rows, playlist clips,
  mixer strips).
* **Cursors**: a collaborator's mouse pointer is shown as an arrow in their colour (the People
  list shows whose colour is whose), on top of the window it points into. It points at the same thing on
  your screen as on theirs, whatever the window sizes, layout, interface scale or Retina / HiDPI
  display: Gridline sends what the pointer is over, not where it is in pixels:
  * in the **playlist**: the track, the song time and the height in the track (also over the track
    headers and the ruler); in the **piano roll**: the key, the time, the pattern and channel
    (also over the keyboard, ruler and velocity lane); in the **channel rack**: the channel row;
    in the **mixer**: the strip;
  * anywhere else: the button, knob, step or other control under the pointer and the spot inside
    it; failing that, the spot in the window.

  Your own scroll and zoom apply, so the cursor sits on the same bar and track even if you are
  zoomed differently. The arrow is **the size of your own mouse pointer** (your system's pointer
  size, also when you made it larger for accessibility), with slightly rounded corners, a thin light
  outline and a soft shadow.

  When that spot is not shown on your screen (a window you have closed, collapsed or behind another
  tab, another pattern in the piano roll, a part you have scrolled away from, a menu, or when your
  collaborator is outside Gridline) the cursor waits a moment where it was and then fades out
  quickly. Crossing a gap between windows, a title bar or a window edge is shorter than that moment,
  so the cursor does not blink. Moving to another window that you have open moves it there without
  fading (a brief crossfade only for a jump across the screen). It dims a little after a few seconds
  without moving. Turn cursors off in the Collaboration panel or in Settings > Collaboration.

  **Windows in front**: a window you have open in front of the one a collaborator points into
  covers their cursor, as if it were behind that window (J-111h): Settings, Project info, the
  Collaboration panel, dialogs, a floating channel rack, mixer or piano roll, a plugin editor,
  popups and menus. Only the part under the window is hidden, cut at its edge; it does not fade in
  and out. When they point into that same window (both of you have Settings open and they point in
  it), their cursor is on top of it. Docked panels side by side never cover each other. While you
  have a confirmation (a modal) open, cursors behind it are dimmed with everything else.
  **Plugin editors**: a cursor in a plugin editor is drawn there only when you have the same
  plugin's editor open (the same channel's instrument, the same mixer slot).
* The **top bar** shows everyone in the project: their picture or initials, a green dot when they
  are online, the latency on hover. Click it to open the Collaboration panel. A collaborator's
  picture arrives when you connect and again whenever they change it; it is kept on your computer
  (with their name and colour), so it also shows while they are offline and after you reopen the
  project.
* **Undo** undoes only your own changes. If a collaborator changed the same thing after you, your
  undo leaves their change alone. This holds in plugin editors too: a knob drag or a drawn shape
  is one step, and your undo leaves a collaborator's knob on the same plugin as it is.
* **Playback is your own.** Pressing Play plays the song on your computer only. To listen along,
  right-click a collaborator in the Collaboration panel and choose **Follow their playhead**: your
  transport follows theirs until you choose it again.
* **What sounds stays on your computer.** Nothing you play, preview or record makes sound on a
  collaborator's computer: the transport, keys you play or hold, pads, the browser preview, a plugin
  playing on its own (Seltmann's pattern, Kerf, the chord preview, Loupe, Shuttle), Stop, recording
  in progress. A collaborator's edits never trigger a sound on yours either (no audition of their
  notes, knobs or samples).
* **Listening aids are yours too**: soloing a mixer track, record arm, input monitoring, a live clip
  you launched, folded tracks and track heights, and listening switches inside Gridline's plugins
  (band solo and listen, Delta, Hold, Latch, display freezes) stay on your computer. Mute is shared,
  because it is part of the mix.
* If a collaborator deletes or changes the channel or sample a plugin of yours is playing on its
  own, it stops cleanly. A recording reaches the others when you stop it, as a loading clip until
  the audio has arrived.

### Audio files

When someone adds an audio file (in the playlist, channel rack, sampler, Seltmann, an impulse
response for the convolver...), it is copied into the project's `Audio` folder and sent to the
others. Until the file has arrived and its checksum matches, the others see a **loading
placeholder** (striped, with the progress) and the clip or sample is silent. Then it plays.
Files are named by their content (SHA-256), so the same file is never stored or sent twice, and
an interrupted transfer continues where it stopped.

## Working offline and catching up

You can keep working on a collaborative project when nobody else is online, or when your computer
is offline. Your changes are kept and sent the next time you are online together.

**There is no server.** A change only travels while two computers that have it are online at the
same time. If you change something and switch your computer off, and your collaborator opens the
project later, they will not see your change until both of you are online together, or until a
third collaborator who already has it is online with them (changes are passed on by anyone who has
them, and they are signed, so nobody in between can alter them).

The Collaboration panel shows, for every person, when you last synced with them and how many of
your changes they have not seen yet. While catching up it shows "Syncing 214 changes".

### Sync review: changes made while apart

Most changes made while apart simply merge. When two people changed **the same thing** while they
were not connected, the newer change is used for now (a deletion wins over an edit), so the
project stays consistent and playable on every computer, and the change pair goes into the
**Sync review** for the project's admin:

* the same property (a knob, a mixer level, a note, a clip position, a name);
* something deleted on one side and edited on the other;
* the same pattern or clip rearranged by both.

Edits made at the same time while you are connected never end up in the review.

The **admin** is the person who made the project collaborative; the admin can hand the role to
someone else (right-click a person in the Collaboration panel > Make admin). For each item the
review shows every side, oldest first: who made it, when, and what it changed ("Volume 78% -> 55%").
**Keep this one** keeps a side; **Preview** lets you hear and see a side first without keeping it;
the buttons at the top keep everything from one person, or the newest or oldest side of every
item. Nothing is thrown away: resolved items move to the **Resolved** tab, where the other side can
still be restored. Everyone else sees "3 changes awaiting the admin's review" and the admin's choice
reaches them like any other change, also if they were offline when it was made.

Times come from each computer's clock. If two clocks disagree by more than a few seconds, times
are shown as "about".

## Design notes: the shared name (J-111f)

* **Where it lives.** The name is `meta/project/name` in the replicated document: a last-writer-wins
  register ordered by the hybrid logical clock and the site id, like every other value, separate
  from the file and folder names. The rest of `meta/project` (id, creator) stays admin-only; the
  name may be written by **any member** (a rename is an ordinary edit, and the admin can still
  settle disputes in the Sync review). A name op that is not a `set` of a valid name (or of nothing,
  which falls back to the file name) is ignored by every peer, so all of them stay identical.
* **Who renamed it** is the author of the register's current value; the notice is shown when the
  name a computer shows changes (live, catch-up, snapshot, undo, a review choice), not for the name
  a join receives with the project.
* **collab.json** caches the name (for lists of projects that are not open) and records which file
  in the folder is the project (`"file"`). Finding the file: that record, else a file named after
  the name, else the only `*.gridline.json` in the folder, else the one whose content Gridline wrote
  last (`written.txt`), else the newest. Folders are identified by the project id in collab.json.
* **File renames** are atomic `rename()` calls in the same folder, done before the content is
  written; a case-only rename on a case-insensitive file system goes through a temporary name.
* **Undo**: the rename is a history step without song changes (`Document::recordMark`); the
  session's undo record for it writes the previous value back only if the register still holds
  this user's rename.
* **Older builds** of this branch read `meta/project/name` already (the name the project was made
  with) and ignore collab.json's `"file"`; they let only the admin write `meta/project`, so they
  would ignore other members' renames. The version gate keeps different builds from collaborating,
  so this never mixes in practice; opening a renamed folder in an older build finds no file under
  the cached name and writes a new one.
* **Races**: a rename that reaches a copy after Leave / Stop sharing is not applied there (it no
  longer syncs); a rename by someone who was removed is refused like any of their changes.

## Design notes: cursors and pictures (J-111g)

* **Size.** Each computer draws collaborators' arrows the size of its own pointer, so everyone sees
  them as large as their own. The height of the system arrow (tip to tail, outline included) comes
  from `host/ui/NativeView.h` `pointerSize`, in window units, and is divided by the interface scale
  (ImGui's logical units; the framebuffer density applies on top). macOS: the visible part of
  `[NSCursor arrowCursor]`'s image (about 18 pt) times System Settings > Accessibility > Display >
  Pointer size (`com.apple.universalaccess` `mouseDriverCursorSize`). Linux and the BSDs:
  `Xcursor.size` in the X resource database (device pixels), else `XCURSOR_SIZE`, else GNOME's
  `cursor-size` (asked once through `gsettings`), else 24; the arrow is taken as 4/5 of that
  nominal size (Adwaita, Yaru, DMZ). Windows: `SM_CYCURSOR` (32 at 100 %), arrow about 0.62 of it.
  Queried at start and every 3 seconds. No library is linked for it (X11 is read through the
  Xlib SDL loaded). `GRIDLINE_DEV_POINTER_H` fixes it (logical units) for screenshots.
* **Shape.** The classic arrow outline with each corner replaced by a curve tangent to both edges
  (radius about 1 to 1.5 arrow units, a little less on the inner corners of the tail), a light
  outline at least a device pixel wide, a dark hairline under it (it carries the outline on light
  panels) and a two-layer shadow; anti-aliased over one device pixel and placed on the device pixel
  grid, so it stays crisp at 1x, 1.5x and 2x.
* **Visibility.** A cursor whose spot is not shown here holds for 0.28 s at its last place before
  its 0.13 s fade starts (offline: at once); back in time, nothing faded. A move to another window
  shown here glides up to 48 px, snaps beyond that, and crossfades over 0.08 s beyond 320 px.
* **Plugin editors.** Presence carries the plugin whose editor the pointer is in (`po`: the channel
  or mixer track, a shared object id; `ps`: the effect slot, -1 for the instrument), because the
  editor windows have one name (`channelsettings`, `fxeditor`) whatever plugin they show. A cursor
  in an editor showing another plugin here is hidden (`other-plugin`). Gridline's plugin editors
  open here that are in front of the window the cursor points into, and are not the editor it is
  in, are cut out of the arrow (it is drawn in up to four clip rectangles around each editor), so it
  is hidden there without fading. J-111h does this for every window: each root window shown here
  (floating windows, dock hosts, dialogs, modals, popups, menus; not child windows, which are inside
  their root, and not windows without a background or input such as tooltips and overlays) that is
  later in the display order than the root of the window the cursor points into, in the same
  viewport, is cut out. The order is the window's dock tree root's display index, so docked panels
  share their host's order and never cover each other. The arrow stays on the foreground draw list;
  behind the top modal, its outline is painted over with the modal's dim colour (`ModalWindowDimBg`
  at the current dim ratio). The crossfading copy uses the window it was over. An installed plugin's own window (outside the main window) is
  reported as `plugin-window` with the plugin and the pointer's fraction of it; Gridline cannot
  draw over a plugin's own interface, so such a cursor is hidden here. (Installed plugins are not
  allowed in collaborative projects today, so this only matters for later versions.)
* **Pictures.** The profile names the picture by its hash (`avatar`); the bytes (up to 64 KB, more
  than a datagram) are asked for with `avreq` and sent as `av` on the reliable bulk stream, checked
  against the hash. Before J-111g nobody asked and the hash was dropped on arrival, so pictures never
  showed. Now a profile (hello, profile change) naming a picture that is not here asks at once and
  again every 3 s until it arrives; pictures are kept in `<config>/collab/peers/` by fingerprint
  and not fetched again. Older builds of this branch neither ask nor answer pictures by hash; the
  version gate keeps them apart.

## Leaving and stopping

Both are in the Collaboration panel under **This project**, and both ask first.

* **Leave project...** (anyone but the admin of others). The others keep working without you; only a
  new invite brings you back. Choose what happens to your copy:
  * **Keep it as a normal project**: the folder stays where it is with the project file and its
    audio. The shared history (`.gridline-collab`) and the other members' names and pictures are
    deleted. The dialog shows both sizes.
  * **Delete it**: the whole folder goes to the system Trash (macOS) or the desktop trash (Linux),
    never deleted outright. On Windows, or when the trash is on another disk, nothing is deleted
    and the folder stays as a normal project; delete it yourself if you want it gone.

  Leaving is announced with a signed "left" record, so everyone's People list shows "Left" (the
  admin's re-invite or anyone's ordinary invite brings you back). If some of your changes haven't
  been confirmed by a collaborator yet, the dialog says how many and offers **Wait for sync**, which
  goes on by itself once someone has them, or **Leave anyway**.
* **The admin** can't simply leave while others are in the project: make someone else admin first
  (the "..." menu next to them), then leave. Alone in the project, Leave is the same as Stop
  sharing.
* **Stop sharing...** (the admin): the project stops syncing for everyone and nobody can join any
  more. A signed "ended" record goes to everyone online; whoever is offline learns it the next time
  they connect to anyone who has it. Each person sees "<admin> stopped sharing <project>" with
  **Keep a private copy...** and **Close project**. The admin's copy becomes a normal project in
  place. A stopped project is never shared again: to work together again, make the project
  collaborative again (a new folder) and invite everyone.

## Removing someone

The admin can remove anyone else: **...** next to them in People > **Remove from project...**. The
confirmation says what happens and takes an optional note, which is shown to them (and kept with
the removal, so everyone in the project can read it). There is no Ctrl+Z for a removal; to bring
someone back, re-invite them.

**Who can.** Only the admin. The removal is a signed record in the project's shared history, like
any change; every computer checks that it was signed by whoever was admin at that point of the
history and ignores it otherwise (a forged or non-admin removal has no effect anywhere). Since there
is only one admin at a time and the history is applied in order, two computers always agree on who
was admin. You can't remove yourself (leave instead); an admin can't hand the admin role to someone
removed.

**What happens on every computer.**

* Nobody connects to them any more: every member refuses their connection with the reason
  "removed" (and the signed proof), nobody relays their changes or introduces them to others, and
  open connections to them are closed. Invites they had stop working; joining with any invite is
  refused, except an admin's **Re-invite** made for them.
* Their changes up to the removal stay, and Sync review still shows them as theirs. The removal
  records how much of their work the admin had; anything they changed after that point (changes
  that were still on the way, or made offline) is dropped on every computer, also on computers
  that had already applied it: those rebuild the project from their history without it (or ask a
  collaborator for a clean copy).
* People lists hide them; **Removed (n)** shows who was removed, by whom, when and why, with
  **Re-invite...** for the admin.

**On their computer.** If they have the project open, a dialog says "You were removed from
<project> by <admin>" with the note. Nothing syncs any more and editing is off; playback goes on.
**Keep a private copy...** saves a normal project (music and audio, no shared history, no
identities) in a new folder and opens it; **Close project** leaves the folder as it is. If they were
offline, they learn it the next time their Gridline connects to anyone in the project, and see the
same dialog. In Your collaborative projects the project then shows **Removed** with **Keep a private
copy...** and **Forget**.

**What can't be taken back.** A removal stops future syncing. It can't reach into their computer:
the copy they already have stays theirs, with everything they had received. There are no shared
group keys in Gridline (every connection has its own keys between two people), so nothing needs
to be changed after a removal.

**Corner cases.**

* Removed while joining: the inviter refuses the handshake; the join fails with the reason.
* The only other member removed: the admin goes on alone; the project stays collaborative (invite
  someone, or Stop sharing).
* Two admins at once can't happen (one admin at a time); a removal and an admin hand-over at the
  same time are ordered by the history, and a removal signed by someone who was no longer admin is
  ignored everywhere.
* Their changes that arrive late (queued, relayed, from a merge after working offline) are dropped
  by the same rule, everywhere.
* The same person on two computers is one member: both are removed.
* Removed, then re-invited: they come back as the same member, with their earlier changes; what
  they did while removed stays dropped.
* Peers on an older Gridline: the removal changed the protocol (version 2), so older builds are
  refused and asked to update.

## What happens to the files

Nothing is ever deleted without asking, and nothing goes away by itself.

| What happens | Stays on this computer | Deleted | What the others see |
|---|---|---|---|
| Close the project or quit | everything; the folder stays collaborative (unsent changes are kept and sent next time; files still arriving go on next time) | nothing (connections and router port openings end) | you go offline |
| Leave > keep | the folder as a normal project: project file and audio | the shared history, the other members' names and pictures, queued changes | "Left" in People |
| Leave > delete | nothing (the folder is in the Trash) | the folder goes to the Trash | "Left" in People |
| Stop sharing (admin) | the admin's folder as a normal project | the admin's shared history and others' details | "<admin> stopped sharing" with keep / close |
| Removed by the admin | the folder as it was (marked removed), until you keep a private copy or forget it | your cached details of the other members | "Removed" in People (admins can re-invite) |
| Abandoned (nobody came back) | everything | nothing | — |
| Folder moved or renamed outside Gridline | the folder | nothing | — |
| Folder copied outside Gridline | both folders | nothing | — |
| Folder deleted outside Gridline | — | (you deleted it) | you go offline |

* **Abandoned.** The panel shows when you last synced with a collaborator. After 30 days without
  anyone, it offers **Make it a normal project...** (for the admin of others this stops sharing for
  everyone). Admins can also remove members who never come back.
* **Moved, renamed, deleted.** Your collaborative projects shows the project as **Missing**, with
  **Locate...** (choose where it is now) and **Forget**.
* **Copied.** A copy of the folder can't sync as you next to the original (both would be the same
  member). Opening it asks: open the original, make the copy a separate private project (its
  shared history is deleted), or use the copy instead of the original.
* **Keep a private copy** and **Make it a normal project** write the project file and its audio
  (`<name>.gridline.json` + `Audio/`); the music is the same, the file has no identities, keys,
  names or history.
* **Forget** (removed, stopped or missing projects) takes a project off the list; a removed or
  stopped folder stays as a normal project without its shared history.
* **Storage.** This project > Files shows the folder's size, its audio and its shared history.
  **Clean up unused audio...** lists and deletes audio files in the folder that nothing refers to any
  more: not the project, not the shared history this computer holds, not undo / redo and not Sync
  review. It shows the size first and never touches anyone else's copy.

## Versions

Collaboration needs the newest Gridline: the version you run must be at least the newest release
that has a download for your computer. Gridline checks this against the project's GitHub releases
at most once an hour (it ignores "Skip this version" and "Remind me later", which only concern the
update popup). If you are behind, the collaboration buttons are greyed out with "Update to Gridline
X.Y.Z to use collaborative features" and an **Update** button that shows the usual update window
(release notes, download, checksum check). Opening a collaborative project while behind shows
"This is a collaborative project. Update to Gridline X.Y.Z to open it."

If the check is not possible (offline, GitHub busy), the last known newest version decides, and
the computers compare their versions when they connect anyway: someone on another version is told
politely ("Sam is on Gridline 0.7.1; update to collaborate"). A project also remembers the newest
Gridline that has written to it and refuses to open in an older one.

If you installed Gridline from a Flatpak or your system's package manager, that store may get a new
release a little later than GitHub. Until it does, the update window offers the official download
from GitHub; you can also wait for your store. A new release that appears while you are working
never interrupts you: a note says you will need it next time you open the project.

## Connecting across networks

Gridline connects the computers directly, in this order:

1. **Direct, on the same network or with IPv6.** Usually just works.
2. **Hole punching** (ICE with STUN). Both computers learn their public address from a STUN
   server and send to each other at the same moment. This works through most home routers and no
   port has to be opened by hand.
3. **Automatic port opening.** If allowed (Settings > Collaboration, on by default), Gridline asks
   your router for a temporary port (UPnP-IGD, NAT-PMP or PCP) and removes it again when the
   project closes.
4. **Through another collaborator.** If two people can't connect but both reach a third person,
   their changes and cursors travel through that person's Gridline. Everything stays end-to-end
   encrypted and signed, so the person in between can't read or alter it.
5. **A TURN relay** you configure in Settings > Collaboration, for networks that block all of the
   above.

How often a direct connection works, honestly: on the same network and between typical home
routers almost always; with one side on a mobile phone hotspot or a corporate or university network
often; when **both** sides are behind carrier-grade NAT (many mobile networks) or strict symmetric
NAT, rarely without a third collaborator or a TURN server. When it fails, the Collaboration panel
says "Couldn't connect directly to Sam. Your network or theirs blocks peer-to-peer connections."
and lists what is left. **Test my connection** (in the panel and in Settings > Collaboration)
reports your router's behaviour, whether automatic port opening works and whether IPv6 is
available.

After a restart, members find each other again by themselves when their routers kept the same
addresses (common), or through any collaborator who is online. Otherwise, use **Reconnect with a
code** in the Collaboration panel: it works like an invite, without making anyone a new member.

The connection runs on its own thread, so it stays up while the window is busy, minimised or
hidden (loading a big project, a modal file dialog). On macOS Gridline also asks the system not to
put it to sleep (App Nap) while a collaborative project is open; the computer itself may still go
to sleep.

## Settings > Collaboration

* **Display name**, **contact email** (optional) and **Share my email with collaborators** (off by
  default).
* **Picture**: PNG or JPEG, chosen with Choose picture... or dropped on the circle. It is cropped to
  a square, made 256 pixels small and saved again without any metadata (location, camera). Without
  a picture your initials are shown on your colour.
* **Colour**: the colour of your cursor on other screens and of your entry in the People list.
* **Fingerprint**: your identity, to compare with a collaborator by phone or chat. **Reset
  identity** makes new keys; you then have to be invited again to every collaborative project.
* **Connection**: collaboration version status, automatic port opening, the STUN server, an
  optional TURN relay (the password is kept in the system's secure store), **Test my connection**.

## Privacy and security

* Project data, changes, files, names, pictures and shared emails travel **only** to the people
  invited to the project, **end-to-end encrypted** (X25519 key exchange, XChaCha20-Poly1305). No
  Gridline server is involved and nothing is sent anywhere else.
* Every change is **signed** by its author's key (EdDSA), so a change passed on by someone else
  can't be forged or altered.
* An invite works once and expires. The connection to the inviter is authenticated by the invite's
  secret and by the inviter's key in the code; later connections between members by their keys.
* Gridline contacts a **STUN server** (by default `stun.l.google.com`; you can change it or leave it
  empty for the same network only) to learn your public address. That server sees your IP address
  and nothing else. **Test my connection** additionally asks `stun.cloudflare.com` and your router.
  A **TURN server**, if you set one, relays the encrypted connection: it sees both IP addresses
  and how much data passes, never the content.
* The codes you send each other contain network addresses (your local and public IP addresses) and,
  for invites, the one-time secret. Send them only to the person they are for.
* Your profile (name, email, picture), your keys and other people's cached names and pictures stay
  in your settings folder (`collab/`), never in a project. A project's history stores only an
  author's id and the name they had at the time; never an email or a picture.
* When a project ends for this computer (you leave, are removed, or sharing stops), Gridline
  deletes what it kept about that project's other members on this computer (cached names and
  pictures, the project's connection records). A normal project made from it (keep, private copy)
  contains no identities at all.

## Limits

* Up to 63 people per project.
* Without a server, changes only travel while two computers that have them are online together.
* Cursors land on the same thing when both sides show it. Over an empty part of a window that
  isn't one of the editors, they land on the same spot in the window instead, which can be off a
  little when the windows have very different sizes. Controls are found by their internal names,
  which follow the interface language: with two different languages, a few controls fall back to
  the spot in the window.
* Transport, playback and the metronome stay on each computer (Follow their playhead is optional).
* Plugins that are not part of Gridline can't be used in a collaborative project.
* Two invites handed out by different members at the very same moment can be given the same
  internal number; Gridline keeps object ids apart by member, so this is rare and only affects
  objects created in that same second.

## Testing with two computers (step by step)

These steps check what can't be tested on one computer. Use two computers (A and B), each with this
build of Gridline and a display name set.

1. **Same network.** A: open the demo song, Collaboration panel > Share this project: check the name,
   Choose... a folder, Make collaborative. Create invite (1 hour), Copy. B: File > Join collaborative project..., paste,
   choose a folder, Join. Expect: B opens the song within seconds, no reply code. Both see the same
   safety code. Both top bars show two people.
2. **Live edits.** A drags a mixer fader; B sees it move while A drags. B adds notes in the piano
   roll; A sees them. A turns a knob in a Gridline plugin (for example the Parametric EQ); B hears
   the change. Both see the other's cursor in the playlist when both have the playlist open.
3. **Audio.** B drags a WAV file from their computer into the playlist. A sees a striped "Loading"
   clip that turns into the waveform and plays.
4. **Undo.** A changes a channel's volume, B renames the same channel, A presses Ctrl+Z: the volume
   goes back, B's name stays, on both computers.
5. **Different networks.** Put B on another network (another house, or a phone hotspot with
   Wi-Fi on the laptop). Repeat steps 1 to 3 with a new invite. If B shows a reply code, send it to
   A and paste it under the invite (Use reply). Expect: connected within a few seconds after the
   reply code.
6. **Phone hotspot on both sides (carrier-grade NAT).** Put both computers on two different phone
   hotspots. Try an invite with the reply code. If it does not connect within a minute, expect the
   message "Couldn't connect directly..." with the options; press Test my connection on both and
   note the router line. Then either add a third computer on a normal home network that both join
   (changes should travel through it: the panel shows "Online through ..."), or set a TURN server in
   Settings > Collaboration on both and try again.
7. **Offline and catch up.** Close the project on B. On A, change something; close A. Open the
   project on B, change the same channel's name; close B. Open both: B shows "Syncing ... changes",
   the admin (A) gets the Sync review with both names, times and authors. Keep B's name: both
   computers show it. In the Resolved tab, restore A's name: both computers show A's name.
8. **Reconnect.** With both projects open, restart Gridline on B and open the project from File >
   Recent collaborative projects. Expect B to reconnect by itself within a few seconds (Online,
   direct). Then quit both, wait a minute and open the project on A, then on B: they reconnect by
   themselves, B is still the same member and "changes not seen yet" goes back to 0. If not, use
   Reconnect with a code on both, and report the log described below.
9. **Versions.** (Optional, needs two builds.) Run an older build on B and join: B is told to
   update, A is told B needs to update; nobody is admitted.
10. **Rename.** A: Collaboration panel > click the name > "Night Drive: take 2" > Enter. Expect on B within a
    second: the notice "A renamed the project to Night Drive: take 2", the window title and panel
    header, and B's file `Night Drive- take 2.gridline.json` (the colon is not allowed in file
    names). Close B; A renames again; open B: B shows the newer name after catching up. Then both
    rename while disconnected (pull the network cable): after reconnecting, the admin gets a Sync
    review item for the name.
11. **Leave.** B: Leave project... > Keep it as a normal project > Leave. A's People list shows
    "Left (1)" with B; B's folder opens as a normal project and has no `.gridline-collab` folder.
12. **Remove.** Invite B again (A: People > Left > Invite again; B joins). With the project open on
    both, A: "..." next to B > Remove from project..., type a note, Remove. Expect within seconds:
    B sees "You were removed from ... by A" with the note and can't edit; music keeps playing if it
    was. B: Keep a private copy... > choose a folder: a normal project opens. A's People list shows
    "Removed (1)". Then B opens the old folder from Your collaborative projects: it shows "Removed".
13. **Removed while offline.** Re-invite B (A: Removed > Re-invite...), B joins and closes the
    project. A removes B. B opens the project: within seconds it shows the removed dialog.

## For scripts and tests

    gridline collab-run --folder DIR --name NAME (--new | --create FILE | --join CODEFILE | --open)
                        [--invite-out FILE] [--seconds N] [--edits host|guest] [--audio WAV]
                        [--report FILE] [--stun HOST:PORT] [--app-net] [--ice-delay S]
    gridline collab-run --test-connection

`collab-run` runs a collaborative session without a window (the integration test runs two of them
on one computer). `--open` reopens the project in DIR, as a later run of the same person, and
reconnects by itself. `--ice-delay S` holds back the joiner's connection by S seconds (a slow
network). `--test-connection` prints the same report as Test my connection.

**Diagnostics.** Start Gridline with `GRIDLINE_COLLAB_DEBUG=1` to log every connection step on
stderr (lines starting with `collab-debug`): ICE states and the chosen addresses, handshake packets
sent, received and ignored, failed sends, links opened and closed and why. For a two-computer test:

    GRIDLINE_COLLAB_DEBUG=1 gridline-app 2> collab.log

`GRIDLINE_COLLAB_TEST_ICE_DELAY=S` delays a join's connection by S seconds (tests only).

**Dev input.** `gridline-app --dev-input FILE` scripts accept `collab profile NAME`, `collab new
FOLDER`, `collab invite FILE [minutes]`, `collab join CODEFILE FOLDER`, `collab open FILE|FOLDER`,
`collab edit CHANNEL`, `collab reconnect-code FILE`, `collab reconnect FILE`, `collab stall
SECONDS`, `collab report FILE`, `collab links`, `collab rename "NAME"` (and, for screenshots,
`collab name-window` (the panel's Share this project, name field focused), `collab share-name
"NAME"`, `collab share-folder DIR`, `collab share` (Make collaborative), `collab rename-window`
(renaming in the panel's header), `collab project-info [rename]`, `collab rename-type "NAME"
[folder]`, `collab rename-ok` and `collab demo-rename "NAME"` with `--show collab-demo`), and raise the `waitfor` signals `collab-active`, `collab-ready`,
`collab-invite-ready`, `collab-joined`, `collab-join-failed`, `collab-connected`,
`collab-all-connected`, `collab-synced`, `collab-members:N`, `collab-channel:NAME` and
`collab-name:NAME` (quote a name with spaces). The `j111_collab_app` and `j111_rename_app` tests
(GUI tests on) run two hidden windows with them. For cursors: `collab point WINDOW KIND A B CX CY` moves this window's pointer to an anchor
(kinds `window`, `item`, `playlist`, `playlist-header`, `playlist-ruler`, `piano`, `piano-keys`,
`piano-ruler`, `piano-velocity`, `rack-row`, `mixer-strip`; A / B may be `chN`, `mxN`, `pat`,
`sel`; CX may be in bars, `2.5bar`), `collab demo-cursor ...` holds the simulated collaborator's
pointer of `--show collab-cursors` there, `collab cursors FILE` reports the cursors shown and what
each is over here, `collab pixel FILE` the colour drawn under one; signals
`collab-cursor:KIND:shown|hidden` and `collab-cursor-window:WINDOW:shown|hidden`. The
`j111d_collab_cursors` test uses them (one window with the demo, then two windows of different
sizes and interface scales). J-111g adds `collab picture FILE|#RRGGBB` (this user's picture; a
colour makes a plain one), `collab panel`, `collab pictures FILE` (each member's picture: the hash
announced, received and kept, whether it is drawn and a pixel of it), `collab open-editor chN`,
`collab demo-cursor-plugin chN|none` (the simulated pointer is in that channel's editor) and `collab
trace-reset` (`minVis` in cursor reports: did it fade since), the signal `collab-picture:NAME`, and
the report fields `holding`, `occluded`, `arrowH` and `pointerSource`. J-111h adds `collab
demo-cursor-edge WINDOW COVER DX` (the simulated pointer in WINDOW, DX px right of COVER's left
edge), `collab window settings|info|none`, `collab open-editor none`, the report field `covered`
(the share of the arrow's box covered here), `collab section-probe` and the app's `windows
[FRAMES]` (window positions and sizes, each frame, to stdout); `j111h_dialog_stable` checks that no
window changes size after it opened.

For removing and leaving: `collab remove NAME [NOTE...]` (admin), `collab reinvite NAME FILE`,
`collab keep-copy FOLDER` (removed / stopped: a private copy there, opened), `collab leave
keep|delete`, `collab stop-sharing`, `collab close`, and for screenshots `collab ui remove NAME |
gone | leave | stop | cleanup` and, with `--show collab-demo-panel`, `collab demo-remove-me [NOTE]`
(the simulated collaborator becomes admin and removes you). Signals: `collab-removed`,
`collab-ended`, `collab-copy-done`, `collab-left`; `collab report` adds `exited`, `exitedBy`,
`exitedReason`, `activeMembers`, `removed` and `left`. `GRIDLINE_TEST_TRASH_DIR` (with
`GRIDLINE_TEST_TRASH_ROOT`, the only folder that may be moved there) replaces the system trash in
tests. The `j111e_collab_remove_app` test runs removal, private copy, leave > keep and leave >
delete with hidden windows.
