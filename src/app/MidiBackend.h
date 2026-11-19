// Gridline — the MIDI device back end behind RtMidi's interface.
//   macOS: CoreMIDI · Linux: ALSA (+ JACK) · Windows: Windows Multimedia (WinMM)   (RtMidi)
//   FreeBSD / OpenBSD / NetBSD: sndio (MidiBackend.cpp; RtMidi has no BSD back end)
// Without any of these (e.g. a BSD without sndio) MIDI devices are unavailable and
// midiBackendName() says why.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "../core/MidiOpen.h"

class RtMidiIn;
class RtMidiOut;

namespace gl {

std::unique_ptr<RtMidiIn> makeMidiIn();
std::unique_ptr<RtMidiOut> makeMidiOut();
// "CoreMIDI", "ALSA", "sndio", ... or "none (<reason>)".
std::string midiBackendName();
// Opens the back end and lists its ports (runs on MidiOpener's background thread). Never
// throws: an RtMidi error, or any other, comes back as the result's error text.
// Test hooks (never set by Gridline itself): GRIDLINE_TEST_MIDI=fail makes it fail as a
// refusing service would, GRIDLINE_TEST_MIDI=hang makes it never answer.
// GRIDLINE_TEST_MIDI=fake (MIDIHP-1119) lists the stand-in devices of midiFakeSetPort instead of
// asking the system, so a test can plug devices in and out while the app runs.
MidiProbeResult probeMidiService();

// MIDIHP-1119: devices plugged in or out while Gridline runs.
// How many times the system reported a change of its MIDI devices: CoreMIDI's setup changed
// notification on macOS, a change of the device nodes in /dev/snd on Linux (ALSA); elsewhere
// always 0 (Windows), and the device lists are compared on a timer.
uint64_t midiSetupChanges();
// True once such reports come (the watcher is up, started by the first probe): the timer that
// compares the device lists anyway (software ports) can then be slow.
bool midiSetupWatched();
// False where listing the devices disturbs the ones open (sndio opens each device to list it):
// devices are then listed at startup and by Rescan only.
bool midiHotplugSupported();

// Test hook GRIDLINE_TEST_MIDI=fake: no system MIDI client is ever created; the device lists
// are the stand-ins below (a dev command plugs them in and out), and a stand-in "opens" while
// it is listed. Any thread.
bool midiFakeMode();
void midiFakeSetPort(bool output, const std::string& name, bool present);
bool midiFakePresent(bool output, const std::string& name);

}  // namespace gl
