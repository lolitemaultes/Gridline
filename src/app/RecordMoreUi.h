// Gridline - K-123 (0.7.7 recording rows, core A): quick punch, capture while armed, start on the
// first note, punch crossfades and the loopback latency check, in every layout (they live in the
// shared record menu, the Recording card and strip, and the Record window).
//
// Quick punch (record.quickPunch): while the transport plays, Record (the button, the record key,
// Smooth's R, Clips' arrangement record) punches in and out as often as wanted; the punches of one
// pass are pieces of one take, and everything recorded until the transport stops is one undo step.
// Capture while armed (record.captureSeconds): every armed track keeps that much of its input
// (engine/ArmedRing, made by the ProgramBuilder, never on the audio thread); "Recover the last pass"
// places what was played without Record, where it was played.
// Start on the first note (record.startOnNote): with Record on, Play waits until a note is played
// (the note records at the start) or an armed track's input reaches record.startLevelDb.
// Punch crossfade (record.punchFadeMs): punched takes replace what the track held and crossfade
// with it at each punch (engine/AudioRecord padPunchTakes, TakeCommitOptions::punchFade).
// Latency check: a guided window measures the round trip through a loopback cable
// (engine/Loopback); the measurement applies while the same devices, rate and buffer play.
#pragma once
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {

class App;
struct RecordedTake;

namespace recmore {

// "312 samples (6.5 ms)".
std::string latencyText(int frames, double sampleRate);

// ---- quick punch ----
bool quickPunch(const App& a);  // the mode is on
// The transport plays and a quick punch recording is open (Record now punches).
bool punchRolling(const App& a);
// Punches so far in this recording (audio takes, or 1 while notes record).
int punches(const App& a);

// ---- start on the first note ----
// App::startPlayback: true when Play is held until the first note or sound (Play again while
// waiting starts at once).
bool holdPlay(App& a);
bool waiting();
void startNow(App& a);
void cancelWait(App& a);
// App::playedNoteOn: a note while waiting starts playback; it records at the take's start.
void noteWhileWaiting(App& a, ObjectId channel, int pitch, int velocity);
struct HeldNote {
  ObjectId channel = kNoId;
  int pitch = 0, velocity = 0;
};
// The notes that started the take (the MIDI recorder takes them once it begins).
std::vector<HeldNote> takeStartNotes();

// ---- capture while armed ----
// Seconds of input kept per armed track (0 = off).
double captureSeconds(const App& a);
// Seconds held for the last pass, and on how many tracks (for the window's readout).
void captureHeld(App& a, double& seconds, int& tracks);
// Places the last pass of every armed track as audio clips (one undo step). Returns the clips placed.
int recoverAudio(App& a);

// ---- punch crossfades ----
double punchFadeSeconds(const App& a);

// ---- latency ----
// The measured round trip that applies to the current devices (0: none, the automatic estimate).
int measuredRoundTrip(const App& a);
void openLatencyCheck(App& a);
bool latencyCheckOpen();

// ---- per frame, menus, window rows ----
// App::processAudioRecording, first: quick punch and capture settings to the engine and recorder,
// the start-on-sound request, a waiting start.
void frame(App& a);
// Dialogs: the latency check window.
void draw(App& a);
// The record menu's rows (K-325): quick punch and the first-note start (Punch and count-in group);
// the punch crossfade, capture while armed and the latency check (its Advanced submenu).
void punchMenuItems(App& a);
void startMenuItems(App& a);
void advancedMenuItems(App& a);
// Settings > Audio, beside the recording offset: the latency readout and Measure...
void latencySettingsRow(App& a);
// The record session code reads these (AudioRecordUi): before committing the takes.
void beforeCommit(App& a, std::vector<RecordedTake>& takes);

// --dev-input "recmore show|quick <0|1>|fade <ms>|capture <s>|startnote <0|1>|startlevel <dB>|recover|
// held|punch|clips <track>|latency show|latency measure <channel>|latency use|latency clear|latency open"
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace recmore
}  // namespace gl
