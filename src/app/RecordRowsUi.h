// Gridline - K-123 (0.7.7 recording rows): the takes list with ratings, solo and audition; the MIDI
// input filter's menu rows; Recover what I just played; their dev commands.
//
// Ratings are song data (engine/TakeLanes setRating): undoable, saved, shared in a collaboration.
// Solo and audition are listening only: the engine plays a copy of the song in which one take plays
// on its track (takes::listenTo); the comp, the song and the undo history are untouched, and nothing
// reaches collaborators. Solo stays until turned off; an audition plays the take from its start and
// ends when the transport stops (the song position goes back to where it was).
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {

class App;
struct Song;

namespace recordrows {

// ---- takes ----
void rate(App& a, int track, int take, int value);  // one undo step ("rate take")
bool soloed(int track, int take);
int soloTrack();
void solo(App& a, int track, int take);  // toggles (another take, or another track's, replaces it)
void audition(App& a, int track, int take);
bool auditioning();
void stopListening(App& a);
// App::playedSong: the engine's copy plays the soloed or auditioned take.
void listenFilter(App& a, Song& played);
// Per frame (the Recording card's draw): an audition ends with the transport.
void frame(App& a);

// The take lane header's own controls, right of its label: the rating dots and the solo mark, from
// `h1.x - 20` leftwards. Returns where they start (the label ends before it); `hov`: the pointer is
// on the header. Remembers what the pointer is on for laneHeaderClick.
float drawLaneHeader(App& a, ImDrawList* dl, int track, int take, ImVec2 h0, ImVec2 h1, bool hov);
// The lane header's label for a header `width` wide: "Take 2", or "2" where the controls need the room.
std::string laneLabel(int take, float width);
// A click on the header: true when it was on a rating dot or the solo mark (handled).
bool laneHeaderClick(App& a, int track, int take);
// The lane menu's rows for one take (Rating, Solo, Audition), before the lanes' own rows.
void laneMenuItems(App& a, int track, int take);
// The Record window's Takes page: every track's takes with their ratings, solo and audition.
void takesList(App& a, bool recording);

// ---- notes ----
// The input filter's rows of the record menu (pitch bend, pressure, program changes).
void filterMenuItems(App& a);
// Recover what I just played (the record menu, the Record window's Notes page). Returns the notes placed.
int recoverPlayed(App& a);
void recoverMenuItem(App& a);

// --dev-input: "rectake list|rate <track> <take> <0..3>|solo <track> <take>|audition <track> <take>|unsolo",
// "recfilter show|<notes|controllers|bend|pressure|program> <0|1>|test <status> <d1>",
// "recover [show]", "midiarm <channel index> <0|1>|show|clear".
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace recordrows
}  // namespace gl
