#include "MidiFile.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include "Automation.h"
#include "Params.h"
#include "PatternAutomation.h"

namespace gl {

namespace {

bool fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}

struct Reader {
  const std::vector<uint8_t>& d;
  size_t p = 0;
  bool ok(size_t n) const { return p + n <= d.size(); }
  uint32_t u32() {
    uint32_t v = (uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) | (uint32_t(d[p + 2]) << 8) | d[p + 3];
    p += 4;
    return v;
  }
  uint16_t u16() {
    uint16_t v = static_cast<uint16_t>((d[p] << 8) | d[p + 1]);
    p += 2;
    return v;
  }
  uint32_t vlq(size_t end) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && p < end; ++i) {
      uint8_t b = d[p++];
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) break;
    }
    return v;
  }
};

void putVlq(std::vector<uint8_t>& o, uint32_t v) {
  uint8_t buf[5];
  int n = 0;
  buf[n++] = v & 0x7F;
  while (v >>= 7) buf[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
  while (n) o.push_back(buf[--n]);
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<uint8_t>(v >> s));
}
void put16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}

}  // namespace

bool readMidiFile(const std::string& path, MidiFileData& out, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(err, "cannot open " + path);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parseMidi(d, out, err);
}

bool parseMidi(const std::vector<uint8_t>& d, MidiFileData& out, std::string* err) {
  Reader r{d};
  // RIFF-wrapped MIDI (.rmi): the SMF sits in the "data" chunk.
  size_t base = 0;
  if (d.size() >= 20 && std::equal(d.begin(), d.begin() + 4, "RIFF") && std::equal(d.begin() + 8, d.begin() + 12, "RMID")) {
    size_t p = 12;
    while (p + 8 <= d.size()) {
      const uint32_t len = d[p + 4] | (d[p + 5] << 8) | (d[p + 6] << 16) | (static_cast<uint32_t>(d[p + 7]) << 24);
      if (std::equal(d.begin() + static_cast<long>(p), d.begin() + static_cast<long>(p) + 4, "data")) {
        base = p + 8;
        break;
      }
      p += 8 + len + (len & 1);
    }
  }
  r.p = base;
  if (!r.ok(14) || !std::equal(d.begin() + static_cast<long>(base), d.begin() + static_cast<long>(base) + 4, "MThd"))
    return fail(err, "not a MIDI file");
  r.p = base + 4;
  uint32_t hlen = r.u32();
  uint16_t format = r.u16(), ntracks = r.u16(), div = r.u16();
  if (div & 0x8000) return fail(err, "SMPTE time division is not supported");
  if (format > 1) return fail(err, "MIDI format 2 (independent sequences) is not supported");
  out = MidiFileData{};
  out.format = format;
  out.division = std::max<int>(1, div);
  r.p = base + 8 + hlen;
  for (int t = 0; t < ntracks && r.ok(8); ++t) {
    std::string id(d.begin() + static_cast<long>(r.p), d.begin() + static_cast<long>(r.p) + 4);
    r.p += 4;
    uint32_t len = r.u32();
    size_t end = std::min(d.size(), r.p + len);
    if (id != "MTrk") {
      r.p = end;
      continue;
    }
    MidiTrack track;
    std::map<int, std::vector<std::pair<int64_t, int>>> open;  // key (ch<<8|pitch) -> (start, vel)
    int64_t tick = 0;
    uint8_t running = 0;
    while (r.p < end) {
      tick += r.vlq(end);
      if (r.p >= end) break;
      uint8_t st = d[r.p];
      if (st < 0x80) {
        if (!running) break;  // data byte without a status: corrupt track
        st = running;         // running status
      } else {
        ++r.p;
        if (st < 0xF0) running = st;
      }
      if (st == 0xFF) {  // meta
        running = 0;
        if (r.p >= end) break;
        uint8_t type = d[r.p++];
        uint32_t ml = r.vlq(end);
        size_t mp = r.p;
        if (mp + ml > end) break;
        const std::string text(d.begin() + static_cast<long>(mp), d.begin() + static_cast<long>(mp + ml));
        if (type == 0x03 && track.name.empty()) track.name = text;
        if ((type == 0x06 || type == 0x07) && !text.empty()) out.markers.push_back({tick, text});
        if (type == 0x51 && ml >= 3) {
          uint32_t us = (uint32_t(d[mp]) << 16) | (uint32_t(d[mp + 1]) << 8) | d[mp + 2];
          if (us) out.tempos.push_back({tick, 60000000.0 / us});
        }
        if (type == 0x58 && ml >= 2) out.timeSigs.push_back({tick, std::max(1, int(d[mp])), 1 << std::min(5, int(d[mp + 1]))});
        r.p = mp + ml;
        if (type == 0x2F) break;
        continue;
      }
      if (st == 0xF0 || st == 0xF7) {  // sysex
        running = 0;
        uint32_t sl = r.vlq(end);
        r.p = std::min(end, r.p + sl);
        continue;
      }
      if (st >= 0xF1) {  // stray system common / realtime bytes: skip their data
        const int skip = st == 0xF2 ? 2 : (st == 0xF1 || st == 0xF3) ? 1 : 0;
        r.p = std::min(end, r.p + static_cast<size_t>(skip));
        continue;
      }
      int hi = st & 0xF0, ch = st & 0x0F;
      int nData = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
      if (r.p + static_cast<size_t>(nData) > end) break;
      int a = d[r.p] & 0x7F, b = nData > 1 ? d[r.p + 1] & 0x7F : 0;
      r.p += static_cast<size_t>(nData);
      if (track.channel < 0) track.channel = ch;
      int key = (ch << 8) | a;
      if (hi == 0x90 && b > 0) {
        open[key].push_back({tick, b});
      } else if (hi == 0x80 || (hi == 0x90 && b == 0)) {
        auto it = open.find(key);
        if (it != open.end() && !it->second.empty()) {
          auto [s0, vel] = it->second.front();
          it->second.erase(it->second.begin());
          track.notes.push_back({s0, std::max<int64_t>(1, tick - s0), a, vel, ch});
        }
      } else if (hi == 0xB0) {
        track.ctls.push_back({tick, a, b, ch});
      } else if (hi == 0xE0) {
        track.ctls.push_back({tick, kMidiPitchBend, a | (b << 7), ch});
      } else if (hi == 0xC0) {
        if (track.program < 0) track.program = a;
        if (track.channelProgram[ch] < 0) track.channelProgram[ch] = a;
      }
    }
    for (auto& [key, v] : open)  // unterminated notes end at the track end
      for (auto& [s0, vel] : v) track.notes.push_back({s0, std::max<int64_t>(1, tick - s0), key & 0xFF, vel, key >> 8});
    std::stable_sort(track.notes.begin(), track.notes.end(), [](const MidiNote& x, const MidiNote& y) {
      return x.start != y.start ? x.start < y.start : x.pitch < y.pitch;
    });
    std::stable_sort(track.ctls.begin(), track.ctls.end(), [](const MidiCtl& x, const MidiCtl& y) { return x.tick < y.tick; });
    if (!track.notes.empty() || !track.ctls.empty()) out.tracks.push_back(std::move(track));
    r.p = end;
  }
  auto byTick = [](const auto& x, const auto& y) { return x.tick < y.tick; };
  std::stable_sort(out.tempos.begin(), out.tempos.end(), byTick);
  std::stable_sort(out.timeSigs.begin(), out.timeSigs.end(), byTick);
  std::stable_sort(out.markers.begin(), out.markers.end(), byTick);
  if (!out.tempos.empty() && out.tempos[0].tick == 0) out.tempo = out.tempos[0].bpm;
  else if (!out.tempos.empty()) out.tempos.insert(out.tempos.begin(), {0, 120.0});
  if (!out.timeSigs.empty()) {
    out.timeSigNum = out.timeSigs[0].num;
    out.timeSigDen = out.timeSigs[0].den;
  }
  return true;
}

void splitTracksByChannel(MidiFileData& m) {
  std::vector<MidiTrack> out;
  for (auto& tr : m.tracks) {
    std::set<int> chans;
    for (const auto& n : tr.notes) chans.insert(n.channel);
    for (const auto& c : tr.ctls) chans.insert(c.channel);
    if (chans.size() <= 1) {
      out.push_back(std::move(tr));
      continue;
    }
    for (int ch : chans) {
      MidiTrack t;
      t.name = tr.name.empty() ? std::string() : tr.name + " ch " + std::to_string(ch + 1);
      t.channel = ch;
      t.program = tr.channelProgram[ch];
      t.channelProgram[ch] = t.program;
      for (const auto& n : tr.notes)
        if (n.channel == ch) t.notes.push_back(n);
      for (const auto& c : tr.ctls)
        if (c.channel == ch) t.ctls.push_back(c);
      if (!t.notes.empty() || !t.ctls.empty()) out.push_back(std::move(t));
    }
  }
  m.tracks = std::move(out);
}

std::vector<uint8_t> encodeMidi(const MidiFileData& data, int format) {
  struct Ev {
    int64_t tick;
    int order;  // meta / program / controllers first, then offs before ons at the same tick
    std::vector<uint8_t> bytes;
  };
  auto meta = [](uint8_t type, const std::string& text) {
    std::vector<uint8_t> b = {0xFF, type};
    putVlq(b, static_cast<uint32_t>(text.size()));
    b.insert(b.end(), text.begin(), text.end());
    return b;
  };
  // Conductor events.
  std::vector<Ev> cond;
  {
    std::vector<MidiTempo> tempos = data.tempos;
    if (tempos.empty() || tempos[0].tick > 0) tempos.insert(tempos.begin(), {0, data.tempo});
    for (const auto& t : tempos) {
      uint32_t us = static_cast<uint32_t>(std::lround(60000000.0 / std::max(1.0, t.bpm)));
      cond.push_back({t.tick, 0, {0xFF, 0x51, 0x03, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)}});
    }
    std::vector<MidiTimeSig> sigs = data.timeSigs;
    if (sigs.empty() || sigs[0].tick > 0) sigs.insert(sigs.begin(), {0, data.timeSigNum, data.timeSigDen});
    for (const auto& s : sigs) {
      int dd = 0;
      while ((1 << dd) < s.den && dd < 6) ++dd;
      cond.push_back({s.tick, 0, {0xFF, 0x58, 0x04, uint8_t(std::clamp(s.num, 1, 255)), uint8_t(dd), 24, 8}});
    }
    for (const auto& m : data.markers) cond.push_back({m.tick, 0, meta(0x06, m.text)});
  }
  auto trackEvents = [&](const MidiTrack& tr) {
    std::vector<Ev> evs;
    if (!tr.name.empty()) evs.push_back({0, -1, meta(0x03, tr.name)});
    const uint8_t tch = static_cast<uint8_t>(std::clamp(tr.channel >= 0 ? tr.channel : (tr.notes.empty() ? 0 : tr.notes[0].channel), 0, 15));
    if (tr.program >= 0) evs.push_back({0, 0, {uint8_t(0xC0 | tch), uint8_t(std::clamp(tr.program, 0, 127))}});
    for (const auto& c : tr.ctls) {
      const uint8_t ch = static_cast<uint8_t>(c.channel & 0x0F);
      if (c.controller == kMidiPitchBend) {
        const int v = std::clamp(c.value, 0, 16383);
        evs.push_back({c.tick, 0, {uint8_t(0xE0 | ch), uint8_t(v & 0x7F), uint8_t(v >> 7)}});
      } else {
        evs.push_back({c.tick, 0, {uint8_t(0xB0 | ch), uint8_t(std::clamp(c.controller, 0, 127)), uint8_t(std::clamp(c.value, 0, 127))}});
      }
    }
    for (const auto& n : tr.notes) {
      uint8_t ch = static_cast<uint8_t>(n.channel & 0x0F);
      evs.push_back({n.start, 2, {uint8_t(0x90 | ch), uint8_t(std::clamp(n.pitch, 0, 127)), uint8_t(std::clamp(n.velocity, 1, 127))}});
      evs.push_back({n.start + std::max<int64_t>(1, n.length), 1, {uint8_t(0x80 | ch), uint8_t(std::clamp(n.pitch, 0, 127)), 0}});
    }
    return evs;
  };
  auto chunk = [](std::vector<Ev> evs) {
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.tick != y.tick ? x.tick < y.tick : x.order < y.order; });
    std::vector<uint8_t> t;
    int64_t last = 0;
    for (const auto& e : evs) {
      putVlq(t, static_cast<uint32_t>(std::max<int64_t>(0, e.tick - last)));
      last = std::max(last, e.tick);
      t.insert(t.end(), e.bytes.begin(), e.bytes.end());
    }
    putVlq(t, 0);
    t.insert(t.end(), {0xFF, 0x2F, 0x00});
    std::vector<uint8_t> o = {'M', 'T', 'r', 'k'};
    put32(o, static_cast<uint32_t>(t.size()));
    o.insert(o.end(), t.begin(), t.end());
    return o;
  };
  std::vector<uint8_t> o = {'M', 'T', 'h', 'd'};
  put32(o, 6);
  if (format == 0) {
    put16(o, 0);
    put16(o, 1);
    put16(o, static_cast<uint16_t>(std::clamp(data.division, 1, 32767)));
    std::vector<Ev> all = cond;
    for (const auto& tr : data.tracks) {
      auto e = trackEvents(tr);
      for (auto& x : e)
        if (x.order != -1) all.push_back(std::move(x));  // track names have no place in a single track
    }
    auto c = chunk(std::move(all));
    o.insert(o.end(), c.begin(), c.end());
    return o;
  }
  put16(o, 1);
  put16(o, static_cast<uint16_t>(data.tracks.size() + 1));
  put16(o, static_cast<uint16_t>(std::clamp(data.division, 1, 32767)));
  auto c = chunk(std::move(cond));
  o.insert(o.end(), c.begin(), c.end());
  for (const auto& tr : data.tracks) {
    auto t = chunk(trackEvents(tr));
    o.insert(o.end(), t.begin(), t.end());
  }
  return o;
}

bool writeMidiFile(const std::string& path, const MidiFileData& data, std::string* err, int format) {
  const std::vector<uint8_t> o = encodeMidi(data, format == 0 ? 0 : 1);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return fail(err, "cannot write " + path);
  f.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
  return static_cast<bool>(f) || fail(err, "write failed");
}

// ---------------------------------------------------------------- song → MIDI

namespace {

// MIDI channel of the k-th exported track: 1..16 without the GM drum channel 10.
int midiChannelFor(size_t k) {
  int c = static_cast<int>(k % 15);
  return c >= 9 ? c + 1 : c;
}

struct Collector {
  const Song& s;
  const MidiExportOptions& o;
  std::map<ObjectId, MidiTrack> tracks;

  bool wanted(ObjectId ch) const { return o.channels.empty() || o.channels.count(ch); }

  // Pattern content [0, limit) placed at `offset`, skipping content before `from` (content ticks).
  void addPattern(const Pattern& p, Tick offset, Tick from, Tick limit) {
    Tick stepT = s.ticksPerStep(p);
    for (const auto& c : s.channels) {
      if (!wanted(c.id)) continue;
      MidiTrack& tr = tracks[c.id];
      tr.name = c.name;
      if (auto it = p.steps.find(c.id); it != p.steps.end() && stepT > 0)
        for (size_t k = 0; k < it->second.size(); ++k) {
          const Tick at = static_cast<Tick>(k) * stepT;
          if (it->second[k] && at >= from && at < limit) tr.notes.push_back({offset + at - from, stepT, 60, it->second[k], 0});
        }
      if (auto it = p.notes.find(c.id); it != p.notes.end())
        for (const auto& n : it->second)
          if (n.start >= from && n.start < limit)
            tr.notes.push_back({offset + n.start - from, std::min(n.length, limit - n.start), n.pitch, n.velocity, 0});
      if (!o.automation) continue;
      for (const auto& lane : p.automation) {
        if (lane.owner != c.id || lane.points.empty()) continue;
        const bool vol = lane.index == static_cast<uint32_t>(ChannelParam::Volume);
        const bool pan = lane.index == static_cast<uint32_t>(ChannelParam::Pan);
        if (!vol && !pan) continue;
        auto desc = describeParam(s, {c.id, lane.index});
        if (!desc) continue;
        auto curve = patternLaneCurve(lane);
        auto ccAt = [&](double t) {
          const double v = desc->fromNormalized(automationToParam(*curve, automationValue(*curve, t)));  // J-173: through the range
          return vol ? static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 127.0))
                     : std::clamp(static_cast<int>(std::lround(v * 64.0 + 64.0)), 0, 127);
        };
        const int cc = vol ? 7 : 10;
        int last = -1;
        auto emit = [&](Tick t) {
          if (t < from || t >= limit) return;
          const int v = ccAt(static_cast<double>(t));
          if (v == last) return;
          last = v;
          tr.ctls.push_back({offset + t - from, cc, v, 0});
        };
        const Tick fine = std::max<Tick>(1, s.ppq / 16);
        const std::vector<AutoPoint> pts = patternClipParamPoints(lane, limit);  // J-173: a looping clip unrolled
        for (size_t k = 0; k < pts.size(); ++k) {
          const AutoPoint& pt = pts[k];
          if (k > 0 && pt.shape != AutoShape::Hold)  // curved segment: sampled
            for (Tick t = pts[k - 1].pos + fine; t < pt.pos; t += fine) emit(t);
          emit(pt.pos);
        }
      }
    }
  }

  MidiFileData finish(Tick shift, Tick end) {
    MidiFileData d;
    d.division = s.ppq;
    d.tempo = s.tempo;
    d.timeSigNum = s.timeSigNum;
    d.timeSigDen = s.timeSigDen;
    size_t k = 0;
    for (const auto& c : s.channels) {
      auto it = tracks.find(c.id);
      if (it == tracks.end() || (it->second.notes.empty() && it->second.ctls.empty())) continue;
      MidiTrack tr = it->second;
      const int ch = midiChannelFor(k++);
      tr.channel = ch;
      for (auto& n : tr.notes) n.channel = ch;
      for (auto& x : tr.ctls) x.channel = ch;
      std::stable_sort(tr.notes.begin(), tr.notes.end(), [](const MidiNote& x, const MidiNote& y) {
        return x.start != y.start ? x.start < y.start : x.pitch < y.pitch;
      });
      std::stable_sort(tr.ctls.begin(), tr.ctls.end(), [](const MidiCtl& x, const MidiCtl& y) { return x.tick < y.tick; });
      d.tracks.push_back(std::move(tr));
    }
    if (shift >= 0 && o.tempoMap) {  // song exports: tempo map from tempo automation
      TempoMap map;
      map.build(s);
      if (!map.constant()) {
        for (const auto& seg : map.segments()) {
          const auto t = static_cast<int64_t>(std::llround(seg.tick)) - shift;
          if (t >= end - shift) break;
          const double bpm = std::round(seg.bpm * 1000.0) / 1000.0;
          if (t <= 0) {
            d.tempos.assign(1, {0, bpm});
          } else if (d.tempos.empty() || std::fabs(d.tempos.back().bpm - bpm) > 1e-6) {
            d.tempos.push_back({t, bpm});
          }
        }
        if (!d.tempos.empty()) d.tempo = d.tempos[0].bpm;
      }
    }
    if (shift >= 0) {
      for (const auto& m : s.markers) {
        if (m.pos < shift || m.pos >= end) continue;
        if (m.timeSigNum > 0 && m.timeSigDen > 0) d.timeSigs.push_back({m.pos - shift, m.timeSigNum, m.timeSigDen});
        if (o.markers && !m.name.empty()) d.markers.push_back({m.pos - shift, m.name});
      }
      if (!d.timeSigs.empty() && d.timeSigs[0].tick > 0) d.timeSigs.insert(d.timeSigs.begin(), {0, s.timeSigNum, s.timeSigDen});
    }
    return d;
  }
};

}  // namespace

MidiFileData patternToMidi(const Song& song, ObjectId pattern, const MidiExportOptions& o) {
  Collector c{song, o, {}};
  if (const Pattern* p = song.pattern(pattern)) c.addPattern(*p, 0, 0, song.patternContentLength(*p));
  return c.finish(-1, 0);
}

MidiFileData songRangeToMidi(const Song& song, Tick start, Tick end, const MidiExportOptions& o) {
  Collector col{song, o, {}};
  for (const auto& c : song.playlist) {
    if (c.kind != ClipKind::Pattern || c.muted || c.start < song.live.areaEnd) continue;
    if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) continue;
    const Pattern* p = song.pattern(c.pattern);
    if (!p) continue;
    const Tick len = song.clipLength(c);
    const Tick a = std::max(c.start, start), b = std::min(c.start + len, end);
    if (b <= a) continue;
    // Content ticks [from, limit) of this clip fall inside the range.
    const Tick from = c.offset + (a - c.start), limit = c.offset + (b - c.start);
    col.addPattern(*p, a - start, from, limit);
  }
  return col.finish(start, end);
}

MidiFileData songToMidi(const Song& song, const MidiExportOptions& o) {
  return songRangeToMidi(song, 0, std::max<Tick>(1, song.lengthTicks()), o);
}

const char* gmProgramName(int program) {
  static const char* const kNames[128] = {
      "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1",
      "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
      "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
      "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Nylon Guitar", "Steel Guitar", "Jazz Guitar",
      "Clean Guitar", "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass",
      "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
      "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
      "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs",
      "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section",
      "Synth Brass 1", "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn",
      "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle",
      "Ocarina", "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead",
      "Fifths Lead", "Bass + Lead", "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad",
      "Halo Pad", "Sweep Pad", "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX", "Brightness FX", "Goblins FX",
      "Echoes FX", "Sci-fi FX", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
      "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
      "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
      "Gunshot"};
  return program >= 0 && program < 128 ? kNames[program] : "";
}

}  // namespace gl
