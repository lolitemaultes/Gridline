// Gridline — MIDI file import. See MidiImport.h.
#include "MidiImport.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "Params.h"
#include "PatternAutomation.h"

namespace gl {

namespace {

constexpr Rgb kTrackColors[] = {0x5A8FD8, 0xD8775A, 0x6EB55E, 0xC9A23F, 0x9A6CD0, 0x4FB3B0, 0xD0607E, 0x8C9A5B};

int firstFreeTrack(const Song& s) {
  for (int t = 0; t < kMaxPlaylistTracks; ++t) {
    bool used = false;
    for (const auto& c : s.playlist) used = used || c.track == t;
    if (!used) return t;
  }
  return 0;
}

std::string pluginOr(const std::string& type, const std::string& fallback) { return findPlugin(type) ? type : fallback; }

}  // namespace

MidiImportResult importMidiIntoSong(Song& song, const MidiFileData& midiIn, const std::string& name, const MidiImportOptions& o) {
  MidiImportResult res;
  MidiFileData midi = midiIn;
  if (midi.format == 0 || midi.tracks.size() == 1) splitTracksByChannel(midi);
  const int ppq = song.ppq;
  auto T = [&](int64_t t) { return midiToSongTicks(t, midi.division, ppq); };
  // ---- tempo / meter ----
  if (o.tempo) {
    auto desc = describeParam(song, songParam(SongParam::Tempo));
    const double first = midi.tempos.empty() ? midi.tempo : midi.tempos[0].bpm;
    if (desc && first > 0) song.tempo = desc->clamp(first);
    if (desc && midi.tempos.size() > 1) {
      AutomationClip a;
      a.id = song.allocateId();
      a.name = "Tempo (" + name + ")";
      a.owner = kSongId;
      a.index = static_cast<uint32_t>(SongParam::Tempo);
      for (const auto& t : midi.tempos) {
        AutoPoint p;
        p.pos = T(t.tick);
        p.value = static_cast<float>(desc->toNormalized(desc->clamp(t.bpm)));
        p.shape = AutoShape::Hold;
        if (!a.points.empty() && a.points.back().pos == p.pos)
          a.points.back() = p;
        else
          a.points.push_back(p);
      }
      res.tempoChanges = static_cast<int>(a.points.size()) - 1;
      song.automations.push_back(a);
      Clip c;
      c.id = song.allocateId();
      c.kind = ClipKind::Automation;
      c.source = a.id;
      c.track = firstFreeTrack(song);
      c.start = 0;
      c.length = a.points.back().pos + song.barTicks();
      song.playlist.push_back(c);
    }
  }
  if (o.timeSignature && !midi.timeSigs.empty()) {
    song.timeSigNum = std::clamp(midi.timeSigs[0].num, 1, 32);
    song.timeSigDen = std::clamp(midi.timeSigs[0].den, 1, 32);
    for (size_t k = 1; k < midi.timeSigs.size(); ++k) {
      Marker m;
      m.pos = T(midi.timeSigs[k].tick);
      m.timeSigNum = std::clamp(midi.timeSigs[k].num, 1, 32);
      m.timeSigDen = std::clamp(midi.timeSigs[k].den, 1, 32);
      song.markers.push_back(m);
    }
  }
  if (o.markers)
    for (const auto& mk : midi.markers) {
      Marker m;
      m.pos = T(mk.tick);
      m.name = mk.text;
      song.markers.push_back(m);
    }
  std::stable_sort(song.markers.begin(), song.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });

  // ---- pattern + channels ----
  if (static_cast<int>(song.patterns.size()) >= kMaxPatterns) {
    res.error = "the project has the maximum number of patterns";
    return res;
  }
  Pattern pat;
  pat.id = song.allocateId();
  pat.name = name;
  pat.color = 0x6E9F4B;
  Tick maxEnd = 0;
  int n = 0;
  for (size_t ti = 0; ti < midi.tracks.size(); ++ti) {
    const MidiTrack& tr = midi.tracks[ti];
    if (!o.tracks.empty() && !o.tracks.count(static_cast<int>(ti))) continue;
    int mch = tr.channel >= 0 ? tr.channel : (tr.notes.empty() ? 0 : tr.notes[0].channel);
    if (!o.midiChannels.empty() && !o.midiChannels.count(mch)) continue;
    std::vector<Note> notes;
    for (const auto& mn : tr.notes) {
      if (!o.midiChannels.empty() && !o.midiChannels.count(mn.channel)) continue;
      Note x(T(mn.start), std::max<Tick>(1, T(mn.start + mn.length) - T(mn.start)), std::clamp(mn.pitch, 0, 127),
             std::clamp(mn.velocity, 1, 127));
      notes.push_back(x);
      maxEnd = std::max(maxEnd, x.start + x.length);
    }
    // Controllers with a Gridline target: CC 7 → volume, CC 10 → pan.
    std::map<uint32_t, std::vector<std::pair<Tick, double>>> lanes;
    for (const auto& c : tr.ctls) {
      if (!o.midiChannels.empty() && !o.midiChannels.count(c.channel)) continue;
      if (o.controllers && c.controller == 7)
        lanes[static_cast<uint32_t>(ChannelParam::Volume)].push_back({T(c.tick), c.value / 127.0});
      else if (o.controllers && c.controller == 10)
        lanes[static_cast<uint32_t>(ChannelParam::Pan)].push_back({T(c.tick), std::clamp((c.value - 64) / 64.0, -1.0, 1.0)});
      else
        ++res.skippedControllers;
    }
    if (notes.empty() && lanes.empty()) continue;
    Channel ch;
    ch.id = song.allocateId();
    std::string nm = tr.name;
    if (nm.empty() && o.programNames && mch == 9) nm = "Drums";
    if (nm.empty() && o.programNames && tr.program >= 0) nm = gmProgramName(tr.program);
    if (nm.empty()) nm = name + " " + std::to_string(n + 1);
    ch.name = nm;
    ch.color = kTrackColors[static_cast<size_t>(n) % (sizeof kTrackColors / sizeof kTrackColors[0])];
    std::string type = o.instrument;
    if (mch == 9) type = pluginOr(o.drumInstrument, o.instrument);
    else if (tr.program >= 0 && tr.program < 8 && o.instrument == "gl.synth") type = pluginOr("gl.keys", o.instrument);
    ch.plugin = defaultPluginState(pluginOr(type, "gl.synth"));
    ch.mixerTrack = song.masterId();
    song.channels.push_back(ch);
    res.channels.push_back(ch.id);
    ++n;
    res.notes += static_cast<int>(notes.size());
    if (!notes.empty()) pat.notes[ch.id] = std::move(notes);
    for (auto& [index, pts] : lanes) {
      auto desc = describeParam(song, {ch.id, index});
      if (!desc) continue;
      std::vector<AutoPoint> points;
      for (const auto& [t, v] : pts) {
        AutoPoint p;
        p.pos = t;
        p.value = static_cast<float>(desc->toNormalized(desc->clamp(v)));
        p.shape = AutoShape::Hold;
        if (!points.empty() && points.back().pos == p.pos)
          points.back() = p;
        else
          points.push_back(p);
        maxEnd = std::max(maxEnd, t + 1);
      }
      setPatternLane(pat, ch.id, index, std::move(points));
      ++res.automationLanes;
    }
  }
  if (res.channels.empty()) {
    res.error = "the MIDI file has no notes on the selected tracks / channels";
    return res;
  }
  const Tick bar = std::max<Tick>(1, song.barTicks());
  pat.bars = static_cast<int>(std::max<Tick>(1, (maxEnd + bar - 1) / bar));
  pat.stepsPerBar = 16;
  song.patterns.push_back(pat);
  res.pattern = pat.id;
  if (o.placeInPlaylist) {
    Clip c;
    c.id = song.allocateId();
    c.pattern = pat.id;
    c.track = firstFreeTrack(song);
    c.start = 0;
    if (static_cast<int>(song.tracks.size()) <= c.track) song.tracks.resize(static_cast<size_t>(c.track) + 1);
    song.playlist.push_back(c);
  }
  return res;
}

int insertMidiNotes(Song& song, ObjectId pattern, ObjectId channel, Tick at, const MidiFileData& midi, int track) {
  Pattern* p = song.pattern(pattern);
  if (!p || !song.channel(channel)) return 0;
  std::vector<Note>& dst = p->notes[channel];
  int added = 0;
  Tick maxEnd = 0;
  for (size_t ti = 0; ti < midi.tracks.size(); ++ti) {
    if (track >= 0 && static_cast<int>(ti) != track) continue;
    for (const auto& mn : midi.tracks[ti].notes) {
      const Tick s = midiToSongTicks(mn.start, midi.division, song.ppq);
      Note x(at + s, std::max<Tick>(1, midiToSongTicks(mn.start + mn.length, midi.division, song.ppq) - s), std::clamp(mn.pitch, 0, 127),
             std::clamp(mn.velocity, 1, 127));
      dst.push_back(x);
      maxEnd = std::max(maxEnd, x.start + x.length);
      ++added;
    }
  }
  std::stable_sort(dst.begin(), dst.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  if (dst.empty()) p->notes.erase(channel);
  const Tick bar = std::max<Tick>(1, song.barTicks());
  p->bars = std::max(p->bars, static_cast<int>((maxEnd + bar - 1) / bar));
  return added;
}

}  // namespace gl
