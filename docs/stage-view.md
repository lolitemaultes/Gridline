# The Stage view

The mixer window has two views: **Mix**, the strips you know, and **Stage**. Switch between them
with **Mix | Stage** at the left of the mixer's toolbar, **View > Stage**, or the command palette
(in the Tracker: the **Stage** page tab, see [In the Tracker](#in-the-tracker)).
Both views edit the same tracks, so the strips, undo, automation and the people you share a
project with all see the same change. The view you used last comes back next time.

On the Stage your tracks stand on a stage seen from where you sit (the orange headset at the
front):

- left and right is the track's **pan**;
- near and far is its **distance**: further back is quieter, a little duller and narrower, with
  more of the room;
- the flat oval under a track is its **stereo width**;
- a track further back is drawn smaller and fainter.

Only track names appear on the stage. The values are in the panel under it.

## Moving tracks

| Do this | To |
| --- | --- |
| Drag a track | Move it (a selection moves together). It lands on one of five distances and sticks to the centre |
| Ctrl (Cmd on a Mac) while dragging | Place it freely, without the snap |
| Shift while dragging | Move finely |
| Drag onto another track or a group | Share that spot (see Groups) |
| Click a group's stack | Open the group (see Groups) |
| Alt while dragging | Drop exactly where you let go (joins a track it still covers) |
| Esc while dragging | Put it back |
| Let go off the stage | Put it back |
| Drag the dots at the ends of the oval | Change the stereo width (one track selected) |
| Wheel over a track | Its level, as its fader's wheel does (Shift: its width) |
| Double-click a track | Open its effects, staying on the Stage (Ctrl/Cmd+double-click: its strip in Mix) |
| Click, Ctrl/Cmd+click, Shift+click | Select, add to the selection, select a range |
| Drag on the empty floor | Draw a selection box: every track it touches is selected as it grows |
| Shift while drawing the box | Add the tracks it touches to the selection |
| Ctrl (Cmd on a Mac) while drawing the box | Flip each track it touches in or out of the selection |
| Esc while drawing the box | Put the selection back as it was |
| Click the empty floor, or Esc | Select nothing: the panel shows the room |
| Drag selected tracks onto a room in the rooms list | Move them all to that room (see Rooms) |
| Right-click a track | Its menu: effects, rename, colour, icon, mute, solo, send to the space, move to room, reset, the mixer track menu |
| Right-click the floor | Presets, Spread out, Arrange by sound, snap, names, compact groups |

While you drag, the tooltip says what the move does ("Further back: quieter, more room"), a
dashed oval shows where the track will stand, and an outline marks where it was.

The selection box stays on screen the whole time you drag it, drawn over the tracks with a clear
outline and a faint fill, and a count beside the pointer says how many tracks are selected. Tracks
light up as the box touches them and go dark again when it shrinks away from them.

### Two tracks never sit on top of each other

When you let go of a track:

1. **It joins** the track or group under the pointer, or the one it covers by at least 30 % of its
   own size. It takes that spot exactly (the same pan and distance). The target lights up while you
   hover it, and the tooltip says so ("Join Kick: centre, near").
2. **It stays where you dropped it** when nothing is in the way.
3. **It eases aside** otherwise: to the nearest free place at the same distance (searching from the
   side away from what was in the way), else one row nearer or further. Overlapping is never where a
   drag ends.

With **Alt** held the track lands exactly where you let go when that is free, and joins whatever it
still overlaps when it is not.

Tracks whose places are close but not the same (for example typed values or automation) are
drawn a little apart, so their tokens never cover each other. Their real places do not change.

## Groups

Tracks that share one spot (the same distance, and a pan within 3 %) form a **group**. A group is
only a picture of positions: nothing extra is saved, so undo, automation and collaboration work
through the tracks' own pan and distance.

A group at rest is **closed**: a small stack of tiles on its spot, the size of one track's tile, so
the stage around it and behind it stays free. You can click, drop, drag and draw a selection box
right next to it, and a track can stand directly behind it.

- The closed stack shows how many tracks are there, with a strip of up to six of their colours
  along its foot, and a name such as **Kick +4** under it. Rest the pointer on it for a moment to
  see every track at that spot.
- **Click it** to open it. The **tray** grows out of the stack: the tracks in a tidy grid of
  same-size tiles, a count on its corner, and a stem down to a small ring that marks the spot.
- A group also opens when you choose tracks that are all in it: a click on a tile, Tab onto one of
  its tracks, or picking it in the Mix view.
- It closes again when you click the empty floor, choose a track outside it, or press Esc.
- Only one group is open at a time.
- **Drag the closed stack** to move the whole group. Right-click it for its tracks, Open the group,
  Arrange by sound and Spread out. A selection box that touches it takes the whole group.
- Trays have 4, 6, 8 or 10 columns and at most three rows. A bigger group shows **+N** in its last
  cell: click it for the rest, and for Arrange.
- A new or imported project puts every track at the default spot (centre, front), so it opens as
  one group. Drag tracks out to place them, or use Spread out or Arrange.
- **Drag a tile out** of the open tray to move only that track. Its cell stays as a dashed outline
  until you let go. A moment after the pointer leaves, the tray closes up, so you can put the track
  down right behind its own group. A group of one becomes a plain tile again.
- **Drag a track onto a group to join it.** Dropped straight onto the closed stack, it joins at
  once. Hold it there a moment and the tray opens, outlined in orange, and you can drop it in. The
  tray stays open while the pointer is over it and closes a moment after it leaves.
- Automation that moves tracks makes groups form and part as it plays, without flicker: a group
  holds together until a member is 6 % away.
- Open or closed is only how this computer shows the stage; nothing about it is saved or shared.
- **Compact groups** in the floor's right-click menu turns this off: every group then keeps its
  tray open above its spot, as in earlier versions.

## The panel

The panel under the stage shows what is selected:

- **One track**: its tile and name, Mute, Solo and Effects, and knobs with words: Left / right,
  Distance (Front to Back, with what that does), Width, Focus, Room (its send into the room) and
  Level with a meter. Every knob has the usual right-click menu (automation, links, typing a
  value).
- **Several tracks**: the knobs move them all from where they are. Left / right and Distance move
  the group and keep its spacing, Width scales each track's own width, Level trims them all. A
  knob reads "Mixed" where the values differ. Spread out spaces only these tracks.
- **Nothing selected**: the room shown. The space's size (the space itself is picked from Space in the toolbar),
  Depth (how far back the stage feels: Gentle, Natural or Strong), Room width, the space's level,
  and the **Spread out** and **Arrange by sound...** buttons. Room width narrows (down to mono) or
  widens (up to 150 %) every track in the room at once; at 100 % each track sounds exactly as its
  own pan and width say.

While you drag a track, the panel shows where the drop will put it.

When the mixer is docked as a narrow column, the panel keeps the tile, the name and the buttons and
leaves out the knobs (they come back when the mixer is wider), and the stage presets are in the
mixer menu under Stage.

## Spread out and Arrange by sound

Both show where every track would go first, as dashed outlines, with **Cancel** and **Apply** in
the panel. Nothing moves until you press Apply, and Apply is one undo step. Esc cancels. With two or
more tracks selected they work on those tracks only.

- **Spread out** guesses nothing: per distance, tracks are spaced evenly from the centre outward,
  left and right in turn, in mixer order. Every track keeps its distance. A crowded row moves every
  other track one step back.
- **Arrange by sound...** works out what each track is, in this order:
  1. what plays on it: the instruments routed to it (a drum machine, a slicer, keys, a synth preset's
     category, sample file names), which works even for a track called "Audio 3";
  2. how it sounded: Gridline listens to each track while the song plays with the Stage open (its
     loudness, brightness, low end, how often it hits and how long it holds);
  3. its name, as a hint only.

  Hover a track in the preview to see why it went there ("Placed as bass: low and steady"). A
  track it is not sure of is marked **?** and stays in the middle for you to place.

## Presets

**Presets** in the toolbar starts with Arrange by sound and Spread out, then six stage presets,
each with a small picture: Band, Electronic, Wide pop, Podcast, Chamber and Lo-fi room. Hovering one
shows where it would put your tracks; clicking it places them, picks a room and its depth, and
adds the Room track when there is none. It is one undo step.

## The space

A new project plays in **No space**: tracks keep the distance and focus you give them, but these change
nothing in the sound yet (no level drop, no duller highs, no narrowing, no reverb). Left / right and
width work as always. The floor is drawn open, without an edge, and the first time you drag a track
back a small hint under **Space** says "Pick a space to hear depth".

Pick a space from **Space** in the toolbar. Each room (see Rooms) has a space of its own. The list starts with No space, then:

| Space | Sounds like |
| --- | --- |
| Small room | Close and dry: short reflections from near walls |
| Studio | A live room: tight, clear and natural |
| Club | A low room with a hard floor: punchy, short and a little dark |
| Chamber | A tall bright room: dense and smooth, kind to strings and voices |
| Hall | A concert hall: warm and wide with a long even tail |
| Big hall | A large hall: deep and spacious, the back of the stage far away |
| Church | Stone and height: a long soft tail that blooms |
| Plate | A bright smooth plate with no walls, sitting behind the sound |
| Open air | Outdoors: distance with almost no tail and only a faint echo |

Each room sets the reverb's size, decay and tone (the room is kept out of the bass, so nothing gets
muddy), where the walls are for the first echoes, how far back the back of the stage sounds and the
level of a new send into it. Hover a space to read what it sounds like. Picking a space is one undo
step.

The first time you pick one, Gridline adds the space's track (called **Room** while the project has one
room, and "<room name> space" once it has more): an ordinary mixer track with Gridline's reverb, fully
wet, and every track on the stage gets a send into it. It is never made before you ask for a space
(picking one, a stage preset, or sending a track to the space). Sends you have not changed follow the
level of the space you pick. Going back to No space mutes the space's track (it is never deleted) and
makes distance and focus silent again, in one undo step.

The Space popover also has Amount (the space track's level) and Back of the stage (Gentle, Natural,
Strong), and opens the space track. Sends into the space keep their level as a track moves back, so the dry
sound falls away while the room stays: that is what makes a track sound further away. Tracks at the
front reach the room a little later (pre-delay), and the first echoes from the walls depend on where
a track stands; both can be switched off in the mixer menu (the ≡ button) under Stage.

## Rooms

A big project can group its mixer tracks into **rooms**, for example one room for each song of an
album, so each song has its own stage and its own space. The rooms column runs down the left of the
Stage in every project: a project with one room lists that room, and **+** adds more. Click a room to
show its stage. Every room always plays: the room shown is only what you are looking at.

**Opening and closing it.** A small tab with an arrow sits on the column's right edge, half way down
the stage. Closed, the column is a thin edge and the arrow points out: click the tab (or the edge) and
the column slides out. Open, the arrow points back: click it and the column slides away again. The
`\` key does the same while the Stage has the keyboard, and so does Mixer menu > Stage > Rooms column.
While rooms are soloed, the closed tab shows a small dot. With interface animations off (or reduced
motion asked for by the system), it opens and closes at once.

**Its width.** Drag the column's right edge to make it wider or narrower; double-click the edge to
put it back as it started. It never takes so much that the stage gets cramped. Drag it narrow enough
and it becomes a slim rail: each room is its little floor in its own colour with its headphones under
it, the room shown is marked, and **+** sits under them. Point at a room in the rail to see its
name, click it to show it, right-click it for its menu, and use its headphones as in the full column.
Drag the edge out again for the full rows. Open or closed and the width are kept on this computer
only: they are never saved in the project or sent to collaborators.

| Do this | To |
| --- | --- |
| **+** at the top of the list | Make a new room: from the room shown (its space, back of the stage, width and echoes, never its tracks or places) or from a template |
| Click a room | Show its stage |
| Double-click a room | Rename it |
| Drag a room up or down | Change the order |
| Right-click a room | Rename, colour, duplicate its settings, move up or down, apply a template, save it as a template, bring tracks in, delete it |
| Click a room's headphones | Solo the room; click other rooms' headphones to add them (see Soloing rooms) |
| Right-click a room's headphones (or Ctrl/Cmd+click) | Hear this room only; again: every room |
| Delete with the pointer over a room | Delete it (asked first; see Deleting a room) |
| Drag selected tracks onto a room | Move them all there; the room lights up and the tooltip says "Move 3 tracks to Glass Coast" |
| **Bring tracks in...** | Pick tracks from the other rooms and move them here (see below) |
| **Templates** | Apply a template to the room shown, save the room as a template, delete one of yours |

A mixer track is in exactly one room. Moving tracks keeps everything about them: their places on the
stage, effects, routing and the channels that play into them. They take on the new room's sound: a
send into the old room's space goes to the new room's space instead (at the new space's level, unless
you had changed it). A track with no send into a space that moves into a room with one gets a send
into it, at the level a new track there gets (a track that only plays through a bus stays dry; the bus
carries it). Their distance follows the new room's back of the stage. A whole drag, or
one Move in the dialog, is one undo step, and moving them back works the same way.

New mixer tracks go into the room the Stage shows.

**Deleting a room.** A room can be deleted only when no tracks are left in it: move them to another
room first (drag them onto it, or use Bring tracks in there). Until then **Delete room...** in its
menu is greyed out and says how many tracks are still in it, and every other way of deleting it
(the Delete key, a collaborator) is refused the same way. The tracks that count are the ones its stage
shows: tracks a channel plays into, or with effects, an input, a name, routing to another track or a
place on the stage. The room's own space track does not count and is deleted with the room; unused
inserts move to the room before it. Gridline asks before deleting, and undo brings the room back with
its space. The last room cannot be deleted.

**Soloing rooms.** Each room's row has headphones, like the channel rack's. Click them to solo the
room: only soloed rooms are heard, with the buses their tracks feed, their spaces and the master.
Click other rooms' headphones to add them, click a lit one to take it out, and right-click (or
Ctrl/Cmd+click) to hear one room only; right-click it again and every room plays. A silenced room's
headphones turn grey, and they glow while the room plays. Soloing rooms is for your computer only:
it is never saved in the project or sent to collaborators, and it makes no undo step. With one room
there is nothing to solo, so the headphones appear once there are two.

**Bring tracks in.** The dialog lists the tracks of every other room, grouped by room. Pick them by
an arrangement (every track its clips play through, with a note when a track is also used by another
arrangement), by a pattern, by the tracks chosen on the mixer, by colour or by name, tick or untick
any of them, and press **Move**. With **Bring the tracks that feed them** on, a bus brings the tracks
that send into it.

**Templates** hold a room's sound only: its space and the space's reverb and level, the back of the
stage, the room width and the echo switches, never tracks or places. Gridline comes with six:
Bedroom demo, Live band, Late club, Concert hall, Stone church and Open field. Your own are saved
as files in the presets folder under `Stage rooms`, and are listed in the browser's Stage rooms
section too (double-click one to apply it to the room shown).

A mixer too small to hold the column beside a stage (a short docked mixer, for example) leaves it
out, edge and tab too, until it is made bigger; when there is room for a stage but not for the rows,
the column opens as the slim rail.

In the **Mix** view a thin line in the room's colour runs along the foot of each strip once a project
has two or more rooms, and so does the toolbar's room button: it shows only the room the Stage shows
(click it again for all rooms).

Collaborators see rooms, their names and settings, and tracks moving between rooms; each person
chooses which room they look at and listen to. A project with one room is saved exactly as before;
a project with two or more rooms needs this version of Gridline or later to open.

## Tracks from the playlist

You can pick tracks for the Stage by what plays in the playlist instead of finding them one by one.
**From playlist** in the Stage's toolbar (also in the floor's right-click menu as **Tracks from
playlist**, and in the mixer menu under Stage when the mixer is narrow) has two sources:

- **From the playlist selection**: the mixer tracks the selected clips play through. An audio clip
  counts its channel's mixer track; a pattern clip counts the mixer track of every channel with notes
  or steps in it. With no clips selected, the selected playlist tracks count instead: their clips, and
  the mixer track an audio track records through or the instrument an instrument track plays.
- **From a marker area**: every clip that plays inside part of the song. The list has the time
  selection (when there is one) and each marker, from it to the next marker (the last one runs to the
  end of the song). **Between two markers...** picks any two. A clip that runs over the edge of the
  area counts; a clip that only ends where the area starts does not.

Muted clips and clips on muted playlist tracks do not play, so they are left out; **Include muted
clips** counts them. **Include the buses they play into** (on at first) brings the buses and groups
the tracks send into, such as a drum bus. The master and a room's space track never count.

Each source opens a list that says what it found before anything changes: how many tracks and their
names (a bus is marked), how many clips they came from, muted clips left out, clips whose channels
play straight into the master, and how many of the tracks are in another room or not on the stage
yet. Then:

| Action | Does |
| --- | --- |
| Select them on the Stage | Selects them, so they light up. If none is in the room shown, the room holding most of them is shown. Drag any of them and they all move together |
| Add to the stage | The ones this stage does not show yet (in another room, or not in use) come into this room and stand in free places across the front, from the centre outward, never on top of another track. The number says how many |
| Move to room | Moves them all to a room, keeping their places, as dragging them onto it does. **New room "Verse"** makes a room named after the marker; **New room...** asks for a name |
| Move together | Spread left to right, Bring forward, Push back, Line up at the front, Gather around the centre, Move left, Move right. They move as a group: a shift stops at the edge of the stage so their spacing stays the same |

Every action is one undo step, and collaborators see it as the same moves made by hand.

**Tracks from the playlist...** in the menu (and **Tracks from the playlist selection...** or **Tracks
from a marker area...** in the command palette) opens the same thing as a panel: choose the source, the
first marker and the last one (or the time selection), see what it finds, and act.

In the playlist the same tracks are a right-click away:

- on a clip (with a selection: every selected clip): **Show on Stage** and **Move to Stage room**;
- on a playlist track's name (with several selected: all of them): the same;
- on a marker: **Show this part on Stage** and **Move to Stage room**, with a new room named after the
  marker;
- on the ruler inside the time selection: **Show the time selection on Stage** and **Move to Stage room**.

Pointing at **Show on Stage** lists the tracks it would show. This works in every layout: the Stage is
the mixer's second view in Studio, Clips, Smooth and the Tracker.

## Focus

Focus is how dry and upfront a track is. Present lifts its presence a little and sends less of it
to the room; Soft does the opposite.

## Pan law

New projects use **Stage placement**: a stereo track moved to the side keeps both of its channels
(its image moves as a whole) instead of losing one. Older projects keep **Balance**, the law they
were made with, so they sound exactly as before. Change it in the mixer menu under Stage > Pan law
(Balance, Constant power, Stage placement).

## In the Tracker

The Tracker has the Stage as a page of its own: the **Stage** tab right after **Mix** in the page
tabs, **Ctrl+Alt+G**, **View > Stage** or the command palette. Mix and Stage are the two views of the
same mixer, so **View > Mixer** (Ctrl+Alt+5) goes back to the strips. The page you used last comes
back next time.

It is the same stage, with the same drags, keys, groups, rooms, controller and MIDI mappings and
collaboration as in the other layouts, drawn the way the Tracker draws everything: square bevelled
boxes and buttons, the stage in a sunken well, numbers in the monospaced face and track numbers in
two-digit hex. A track stands on a flat diamond (its stereo width) and its width handles are small
squares.

Instead of the toolbar and the panel under the stage, a column at the right holds:

- **Arrange**: Presets, the space (Studio, Hall and so on), Spread out, By sound (Arrange by
  sound), Playlist (tracks from the playlist), Rooms (the rooms column), Menu and Routing. The
  popovers they open come out of the column's left edge, beside the stage.
- **Track** (or **Tracks**, or **Room** with nothing selected): the name, Mute, Solo and Effects.
- **Place**: Left / right, Distance, Width and Focus as value bars (several tracks: arrows that move
  them all; the room: its space size, depth and width).
- **Level**: the fader, the send into the space and the meter.
- **Groups**: each spot two or more tracks share, with where it stands. Click one to select its
  tracks.

The **-** button on a box folds it to its title row, which then shows its value in short; **+**
opens it again. On a small window the column folds Arrange first, then Level, Track and Place,
until the groups have room for six rows. A box you fold or open yourself stays that way. The
column takes a quarter of the width at most.

The Stage in the other layouts is unchanged.

## Keys

With the mixer window focused in its Stage view:

| Key | Does |
| --- | --- |
| Left / Right | Pan the selected tracks (Shift: finely, Ctrl: in big steps) |
| Up / Down | One row further back or nearer (Shift: finely) |
| Alt+Up / Alt+Down | Level |
| [ and ] | Narrower, wider |
| Tab / Shift+Tab | The next or previous track |
| M, S | Mute, solo |
| 0 | Put the position back (centre, front) |
| Enter | Rename |
| Ctrl+A, Ctrl+D | Select every track on the stage, deselect |

A held key is one undo step. All of them can be changed under Options > Keyboard shortcuts
(Stage).

## Which tracks are shown

The stage shows the room's tracks in use: with a channel, effects, an audio input, a name of their own
or a place on the stage. The master is you, and a space track is the space itself, so neither is a
token. Mixer menu > Stage > Every insert track shows them all.

## In other programs

The stage's values are saved in the project only when they are not the defaults, so projects that
never use the Stage are unchanged. When a project is exported as an `.flp` file, distance is written as
level, an EQ cut and width, room sends as plain sends at their level for the track's distance,
and focus, the pan law and distance automation are listed in the export report. Each track is written
with its own room's settings; room width is left out and listed in the report. MIDI export leaves them out.
