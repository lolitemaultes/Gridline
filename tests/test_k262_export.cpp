// Gridline: K-262, note vibrato in the exports: a MIDI file carries it as pitch bend that follows
// vib::value (with a bend range wide enough for it, and on the note's own member channel in MPE);
// the FL export writes it as slide notes (sampled) and says so; notes without vibrato export as
// before; edits that copy, move or transpose notes keep it.
#include <cmath>
#include <cstdio>
#include <map>
#include "Check.h"
#include "core/MidiFile.h"
#include "core/NoteBend.h"
#include "core/NoteVibrato.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/flp/FlpExport.h"

using namespace gl;

namespace {

Song song() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 2;
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return s;
}

Note vibNote(Tick start, Tick len, int pitch, float depth, float rate) {
  Note n(start, len, pitch, 100);
  n.vib.depth = depth;
  n.vib.rate = rate;
  n.vib.fadeIn = 0.2f;
  return n;
}


}  // namespace

int main() {
  {
    Song s = song();
    const ObjectId ch = s.channels[0].id;
    const Note n = vibNote(0, 192, 60, 150, 5);  // a semitone and a half: inside the default range of 2
    s.patterns[0].notes[ch] = {n, Note(192, 96, 64, 100)};
    MidiFileData m = patternToMidi(s, s.patterns[0].id);
    int range = -1, events = 0;
    double worst = 0;
    for (const auto& tr : m.tracks) {
      for (const auto& c : tr.ctls)
        if (c.controller == 6) range = c.value;
      for (const auto& c : tr.ctls) {
        if (c.controller != kMidiPitchBend || c.tick >= 192) continue;
        ++events;
        const double semis = (c.value - 8192) / 8192.0 * std::max(2, range);
        worst = std::max(worst, std::fabs(semis - bend::pitchAt(n, static_cast<double>(c.tick), s.ppq, s.tempo)));
      }
    }
    std::printf("# midi: range %d, %d bend events in the note, worst %.4f semitones\n", range, events, worst);
    expect((range == -1 || range >= 2) && events > 40, "the vibrato goes out as pitch bend events (the default range of 2 is not announced)");
    expect(worst < 0.01, "each event is vib::value at its tick");
    // A deep vibrato asks for a wider range.
    s.patterns[0].notes[ch][0].vib.depth = 400;
    MidiFileData wide = patternToMidi(s, s.patterns[0].id);
    int r2 = -1;
    for (const auto& tr : wide.tracks)
      for (const auto& c : tr.ctls)
        if (c.controller == 6) r2 = c.value;
    expect(r2 >= 4, "a 4 semitone vibrato announces a bend range of at least 4");
    // Without vibrato: no pitch bend at all (as before).
    s.patterns[0].notes[ch][0].vib = NoteVibrato{};
    MidiFileData none = patternToMidi(s, s.patterns[0].id);
    int nb = 0;
    for (const auto& tr : none.tracks)
      for (const auto& c : tr.ctls) nb += c.controller == kMidiPitchBend;
    expect(nb == 0, "notes without vibrato or bends write no pitch bend");
  }
  {
    // MPE: the vibrato on the note's own member channel.
    Song s = song();
    const ObjectId ch = s.channels[0].id;
    s.patterns[0].notes[ch] = {vibNote(0, 192, 60, 50, 6), vibNote(0, 192, 67, 0, 6)};
    MidiExportOptions o;
    o.mpe = 2;
    MidiFileData m = patternToMidi(s, s.patterns[0].id, o);
    std::map<int, int> perChannel;
    for (const auto& tr : m.tracks)
      for (const auto& c : tr.ctls)
        if (c.controller == kMidiPitchBend) ++perChannel[c.channel];
    int moving = 0;
    for (const auto& [mc, k] : perChannel) moving += k > 10 ? 1 : 0;
    expect(moving == 1, "in MPE only the vibrato note's member channel moves");
  }
  {
    // FL export: written as slides, counted in the report.
    Song s = song();
    const ObjectId ch = s.channels[0].id;
    s.patterns[0].notes[ch] = {vibNote(0, 192, 60, 60, 5), Note(192, 96, 62, 100)};
    std::vector<uint8_t> out;
    flp::ExportReport rep;
    flp::ExportOptions opt;
    const bool ok = flp::exportFlp(s, out, rep, opt);
    expect(ok && rep.counts["notes: vibrato written as slide notes (approximated)"] == 1, "FL export writes the vibrato as slide notes and says so");
    expect(rep.counts["notes: pitch bend lines written as slide notes"] == 1, "through the bend line's slide conversion");
  }
  {
    // Edits that copy notes keep the vibrato (the Tracker and piano roll edits copy the note).
    Note n = vibNote(10, 96, 60, 30, 5);
    Note moved = n;
    moved.start += 48;
    moved.pitch += 7;
    expect(moved.vib == n.vib && moved.hasProps(), "a moved, transposed copy keeps its vibrato");
    // Project round trip of a vibrato-only note: it is a note with properties.
    Song s = song();
    s.patterns[0].notes[s.channels[0].id] = {n};
    Song back;
    expect(songFromJson(songToJson(s), back) && back.patterns[0].notes[s.channels[0].id][0] == n, "saved and read back unchanged");
  }
  return finish();
}
