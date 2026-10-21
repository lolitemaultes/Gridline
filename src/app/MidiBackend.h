// Gridline — the MIDI device back end behind RtMidi's interface.
//   macOS: CoreMIDI · Linux: ALSA (+ JACK) · Windows: Windows Multimedia (WinMM)   (RtMidi)
//   FreeBSD / OpenBSD / NetBSD: sndio (MidiBackend.cpp; RtMidi has no BSD back end)
// Without any of these (e.g. a BSD without sndio) MIDI devices are unavailable and
// midiBackendName() says why.
#pragma once
#include <memory>
#include <string>

class RtMidiIn;
class RtMidiOut;

namespace gl {

std::unique_ptr<RtMidiIn> makeMidiIn();
std::unique_ptr<RtMidiOut> makeMidiOut();
// "CoreMIDI", "ALSA", "sndio", ... or "none (<reason>)".
std::string midiBackendName();

}  // namespace gl
