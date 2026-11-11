// Gridline — the MIDI device back end behind RtMidi's interface.
//   macOS: CoreMIDI · Linux: ALSA (+ JACK) · Windows: Windows Multimedia (WinMM)   (RtMidi)
//   FreeBSD / OpenBSD / NetBSD: sndio (MidiBackend.cpp; RtMidi has no BSD back end)
// Without any of these (e.g. a BSD without sndio) MIDI devices are unavailable and
// midiBackendName() says why.
#pragma once
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
MidiProbeResult probeMidiService();

}  // namespace gl
