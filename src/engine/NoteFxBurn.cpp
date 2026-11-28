// Gridline — offline note processor rendering (see NoteFxBurn.h).
#include "NoteFxBurn.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include "../core/Lanes.h"
#include "../core/MenuOps.h"
#include "../core/Params.h"
#include "../core/StepNotes.h"
#include "AudioEngine.h"
#include "Freeze.h"
#include "NoteFx.h"
#include "Program.h"

namespace gl {

namespace {
constexpr int kSpt = 8;      // samples per tick of the offline clock (exact tick ↔ sample mapping)
constexpr int kBlock = 64;   // offline block size (samples)

struct Proc {
  std::unique_ptr<NoteFx> fx;
  PluginState state;
  NoteFxAux aux;
};
}  // namespace

std::vector<Note> burnNoteFx(const Song& song, ObjectId channel, const std::vector<Note>& notes, Tick length) {
  const Channel* ch = song.channel(channel);
  if (!ch) return notes;
  std::vector<Proc> chain;
  for (const auto& st : ch->noteFx)
    if (auto fx = createNoteFx(st.type)) {
      Proc p;
      p.fx = std::move(fx);
      p.state = st;
      prepareNoteFxAux(st, p.aux);
      chain.push_back(std::move(p));
    }
  if (chain.empty()) return notes;

  // Input events in offline samples.
  struct Ev {
    int64_t at;
    NoteMsg m;
  };
  std::vector<Ev> evs;
  uint32_t id = 1;
  Tick end = length;
  for (const auto& n : notes) {
    NoteMsg on;
    on.kind = NoteMsg::On;
    on.id = id++;
    on.pitch = static_cast<float>(n.pitch + n.fine / 100.0);
    on.detune = static_cast<float>(n.fine / 100.0);
    on.velocity = std::clamp(n.velocity / 127.0f, 0.0f, 1.0f);
    on.color = static_cast<int16_t>(n.color);
    on.pan = n.pan / static_cast<float>(kNotePanMax);
    on.release = n.release / 127.0f;
    on.modX = n.modX / static_cast<float>(kNoteModMax);
    on.modY = n.modY / static_cast<float>(kNoteModMax);
    NoteMsg off = on;
    off.kind = NoteMsg::Off;
    evs.push_back({static_cast<int64_t>(n.start) * kSpt, on});
    evs.push_back({static_cast<int64_t>(n.start + std::max<Tick>(1, n.length)) * kSpt, off});
    end = std::max(end, n.start + n.length);
  }
  std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
    return a.at < b.at || (a.at == b.at && a.m.kind == NoteMsg::Off && b.m.kind != NoteMsg::Off);
  });
  const Tick tail = std::max<Tick>(1, song.barTicks());
  const int64_t total = static_cast<int64_t>(end + tail) * kSpt;

  NoteBuffer bufA, bufB;
  uint32_t idSeq = 0;
  struct Open {
    Tick start;
    NoteMsg m;
  };
  std::map<uint32_t, Open> open;
  std::vector<Note> out;
  auto close = [&](uint32_t key, Tick at) {
    auto it = open.find(key);
    if (it == open.end()) return;
    const NoteMsg& m = it->second.m;
    Note n;
    n.start = it->second.start;
    n.length = std::max<Tick>(1, at - n.start);
    const double p = static_cast<double>(m.pitch);
    n.pitch = std::clamp(static_cast<int>(std::lround(p - static_cast<double>(m.detune))), 0, 127);  // J-180: the key keeps its fine pitch
    n.fine = std::clamp(static_cast<int>(std::lround((p - n.pitch) * 100.0)), -kNoteFineMax, kNoteFineMax);
    n.velocity = std::clamp(static_cast<int>(std::lround(m.velocity * 127.0f)), 1, 127);
    n.color = std::clamp(static_cast<int>(m.color), 0, kNoteColors - 1);
    n.pan = std::clamp(static_cast<int>(std::lround(m.pan * kNotePanMax)), -kNotePanMax, kNotePanMax);
    n.release = std::clamp(static_cast<int>(std::lround(m.release * 127.0f)), 0, 127);
    n.modX = std::clamp(static_cast<int>(std::lround(m.modX * kNoteModMax)), 0, kNoteModMax);
    n.modY = std::clamp(static_cast<int>(std::lround(m.modY * kNoteModMax)), 0, kNoteModMax);
    if (n.start < length || length <= 0) out.push_back(n);
    open.erase(it);
  };

  size_t next = 0;
  for (int64_t clock = 0; clock < total; clock += kBlock) {
    NoteBuffer* in = &bufA;
    NoteBuffer* outb = &bufB;
    in->clear();
    while (next < evs.size() && evs[next].at < clock + kBlock) {
      NoteMsg m = evs[next].m;
      m.offset = static_cast<int>(evs[next].at - clock);
      in->push(m);
      ++next;
    }
    NoteFxContext ctx;
    ctx.tempo = song.tempo;
    ctx.ppq = song.ppq;
    ctx.sampleRate = song.tempo * song.ppq / 60.0 * kSpt;
    ctx.from = 0;
    ctx.len = kBlock;
    ctx.anchorTick = 0;
    ctx.clock = clock;
    ctx.spt = kSpt;
    ctx.playing = true;
    ctx.idCounter = &idSeq;
    for (auto& p : chain) {
      outb->clear();
      p.fx->process(p.state, p.aux, ctx, *in, *outb);
      outb->sort();
      std::swap(in, outb);
    }
    for (int e = 0; e < in->count; ++e) {
      const NoteMsg& m = in->ev[e];
      if (m.target >= 0) continue;  // sent to another channel
      const Tick at = static_cast<Tick>((clock + m.offset) / kSpt);
      if (m.kind == NoteMsg::On) {
        close(m.id, at);  // a repeated id restarts the note
        open[m.id] = Open{at, m};
      } else if (m.kind == NoteMsg::Off) {
        close(m.id, at);
      } else {
        std::vector<uint32_t> keys;
        for (const auto& [k, v] : open) keys.push_back(k);
        for (uint32_t k : keys) close(k, at);
      }
    }
  }
  std::vector<uint32_t> keys;
  for (const auto& [k, v] : open) keys.push_back(k);
  for (uint32_t k : keys) close(k, static_cast<Tick>(total / kSpt));
  ops::NoteSel sel(out.size(), false);
  ops::sortNotes(out, sel);
  return out;
}

bool burnNoteFxToPattern(Document& d, ObjectId pattern, ObjectId channel) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  const Channel* ch = s.channel(channel);
  if (!p || !ch || ch->noteFx.empty()) return false;
  // Steps play too, as the piano roll shows them (K-438: their pitch, fine pitch and shift, and a
  // lane's own grid and loop; each step used to go in at its plain grid place).
  std::vector<Note> in;
  if (lanes::ownGrid(*p, channel)) {
    in = lanes::rowAsNotes(s, *p, channel);
    if (auto it = p->notes.find(channel); it != p->notes.end()) in.insert(in.end(), it->second.begin(), it->second.end());
  } else {
    in = stepnotes::view(s, *p, channel);
  }
  for (auto& n : in) n.step = false;  // (the processed notes are plain notes)
  if (in.empty()) return false;
  auto out = burnNoteFx(s, channel, in, s.patternLength(*p));
  const int processors = static_cast<int>(ch->noteFx.size());
  d.beginGroup("burn note processors to pattern");
  d.setSteps(pattern, channel, {});
  d.setNotes(pattern, channel, out, "burn note processors to pattern");
  for (int k = processors - 1; k >= 0; --k) d.removeNoteFx(channel, k);
  d.endGroup();
  return true;
}


// ---------------------------------------------------------------- K-438: instrument sequencers

bool sequencerOn(const Song& song, ObjectId channel) {
  const Channel* c = song.channel(channel);
  const PluginInfo* info = c ? findPlugin(c->plugin.type) : nullptr;
  if (!info) return false;
  for (int k : info->sequencerParams)
    if (k >= 0 && static_cast<size_t>(k) < c->plugin.params.size() && c->plugin.params[static_cast<size_t>(k)] >= 0.5f) return true;
  return false;
}

bool sequencerPartsOn(const Song& song, ObjectId channel) {
  const Channel* c = song.channel(channel);
  const PluginInfo* info = c ? findPlugin(c->plugin.type) : nullptr;
  if (!info || info->seqParts.outParam < 0) return false;
  auto on = [&](int k) { return k >= 0 && static_cast<size_t>(k) < c->plugin.params.size() && c->plugin.params[static_cast<size_t>(k)] >= 0.5f; };
  return on(info->seqParts.arpOn) && on(info->seqParts.clipOn);
}

std::vector<Note> printSequence(const Song& song, ObjectId pattern, ObjectId channel, SeqPart part) {
  std::vector<Note> out;
  if (!song.pattern(pattern) || !song.channel(channel)) return out;
  // The song a freeze of the channel plays (the channel and what feeds it notes), its MIDI output
  // read here instead of going to listeners.
  Song x = freeze::renderSong(song, freeze::channelUnit(channel));
  for (auto& c : x.channels) {
    if (c.id != channel) continue;
    c.midiOutPort = -1;
    const PluginInfo* info = findPlugin(c.plugin.type);
    if (!info || info->seqParts.outParam < 0) continue;
    // A two-part sequencer sends only the part printed (the render copy's MIDI Out setting).
    const PluginInfo::SequencerParts& sp = info->seqParts;
    auto on = [&](int k) { return k >= 0 && static_cast<size_t>(k) < c.plugin.params.size() && c.plugin.params[static_cast<size_t>(k)] >= 0.5f; };
    if (part == SeqPart::Whole) part = on(sp.arpOn) ? SeqPart::Arpeggiator : SeqPart::ClipPlayer;
    if (static_cast<size_t>(sp.outParam) < c.plugin.params.size())
      c.plugin.params[static_cast<size_t>(sp.outParam)] = part == SeqPart::Arpeggiator ? sp.arpOut : sp.clipOut;
  }
  constexpr int kRate = 44100, kBlock = 64;
  ProgramBuilder builder;
  builder.setSampleRate(kRate);
  auto prog = builder.build(x);
  const ProgPattern* pe = prog->pattern(pattern);
  const Tick end = pe ? pe->length : 0;
  if (end <= 0) return out;
  const TempoMap& map = prog->flatMap;  // patterns play at the song tempo
  const int64_t total = static_cast<int64_t>(std::ceil(map.samplesBetween(0.0, static_cast<double>(end), kRate)));
  AudioEngine eng(kRate);
  eng.setProgram(prog);
  eng.setOffline(true);
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(pattern);
  eng.play();
  const std::shared_ptr<ChannelRuntime> rt = builder.channelRuntime(channel);
  if (!rt || !rt->gen) return out;
  struct Open {
    Tick start = 0;
    int velocity = 100;
    bool on = false;
  };
  Open open[128];
  auto tickOf = [&](int64_t frame) {
    return std::clamp<Tick>(static_cast<Tick>(std::llround(map.tickAfterSamples(0.0, static_cast<double>(frame), kRate))), 0, end);
  };
  auto close = [&](int key, Tick at) {
    Open& o = open[key];
    if (!o.on) return;
    if (at > o.start) out.push_back(Note(o.start, at - o.start, key, o.velocity));
    o.on = false;
  };
  // A slide to a new key starts the new note before the old one ends (both in one instant): the old
  // note then lasts a little into the new one, so the notes overlap and slide when played back.
  const Tick overlap = std::max<Tick>(1, song.ppq / 16);
  Tick lastOn = -1;
  int lastKey = -1;
  std::vector<float> L(kBlock), R(kBlock);
  Generator::MidiOutEvent ev[512];
  for (int64_t pos = 0; pos < total; pos += kBlock) {
    const int n = static_cast<int>(std::min<int64_t>(kBlock, total - pos));
    eng.process(L.data(), R.data(), n);
    const int count = rt->gen->takeMidiOut(ev, 512);
    for (int k = 0; k < count; ++k) {
      const uint8_t st = ev[k].s & 0xF0;
      const int key = ev[k].a & 0x7F;
      const Tick at = tickOf(pos + std::clamp(ev[k].offset, 0, n));
      if (st == 0x90 && ev[k].b > 0) {
        close(key, at);  // a repeated key ends the note before it
        if (at >= end) continue;
        open[key] = {at, std::clamp(static_cast<int>(ev[k].b), 1, 127), true};
        lastOn = at, lastKey = key;
      } else if (st == 0x80 || st == 0x90) {
        close(key, at == lastOn && key != lastKey ? std::min(end, at + overlap) : at);
      }
    }
  }
  for (int key = 0; key < 128; ++key) close(key, end);
  ops::NoteSel sel(out.size(), false);
  ops::sortNotes(out, sel);
  return out;
}

bool printSequenceToPattern(Document& d, ObjectId pattern, ObjectId channel, SeqPart part) {
  const Song& s = d.song();
  const Channel* ch = s.channel(channel);
  if (!s.pattern(pattern) || !ch || !sequencerOn(s, channel)) return false;
  std::vector<Note> notes = printSequence(s, pattern, channel, part);
  if (notes.empty()) return false;
  const PluginInfo* info = findPlugin(ch->plugin.type);
  std::vector<int> seq = info ? info->sequencerParams : std::vector<int>{};
  if (info && info->seqParts.outParam >= 0 && part == SeqPart::ClipPlayer && sequencerPartsOn(s, channel))
    seq = {info->seqParts.clipOn};  // the arpeggiator goes on playing the written notes
  const int processors = static_cast<int>(ch->noteFx.size());
  d.beginGroup("write the sequence to the piano roll");
  d.setSteps(pattern, channel, {});
  d.setNotes(pattern, channel, notes, "write the sequence to the piano roll");
  for (int k : seq) d.setParam(channelPluginParam(channel, k), 0.0, false);
  for (int k = processors - 1; k >= 0; --k) d.removeNoteFx(channel, k);
  d.endGroup();
  return true;
}

}  // namespace gl

namespace gl {

// ---------------------------------------------------------------- K-438: installed plugins' MIDI output

ObjectId midiOutputTarget(const Song& song, ObjectId channel) {
  const int ci = song.channelIndex(channel);
  if (ci < 0) return kNoId;
  const auto listeners = midiPortListeners(song);
  if (static_cast<size_t>(ci) < listeners.size())
    for (int l : listeners[static_cast<size_t>(ci)])
      if (l >= 0 && static_cast<size_t>(l) < song.channels.size()) return song.channels[static_cast<size_t>(l)].id;
  return channel;
}

ObjectId writeMidiOutputPattern(Document& d, ObjectId like, ObjectId source, ObjectId target, const std::vector<Note>& notes,
                                const std::string& label) {
  const Song& s = d.song();
  const Channel* src = s.channel(source);
  if (notes.empty() || !src || !s.channel(target)) return kNoId;
  const Pattern* lp = s.pattern(like);
  int bars = lp ? std::max(1, lp->bars) : 1;
  const int steps = lp ? lp->stepsPerBar : 16;
  const int meterNum = lp ? lp->meterNum : 0, meterDen = lp ? lp->meterDen : 0;
  const Tick bar = lp ? s.patternBarTicks(*lp) : s.barTicks();
  Tick last = 0;
  for (const Note& n : notes) last = std::max(last, n.start + n.length);
  if (bar > 0) bars = std::max<int>(bars, static_cast<int>((last + bar - 1) / bar));
  const std::string name = src->name + " MIDI output";
  d.beginGroup(label);
  const ObjectId pat = d.addPattern(name, std::min(bars, kMaxPatternBars), steps, -1, meterNum, meterDen);
  if (pat != kNoId) d.setNotes(pat, target, notes, label);
  d.endGroup();
  return pat;
}

ObjectId printMidiOutputToPattern(Document& d, ObjectId pattern, ObjectId channel) {
  const Song& s = d.song();
  if (!s.pattern(pattern) || !s.channel(channel)) return kNoId;
  const std::vector<Note> notes = printSequence(s, pattern, channel);
  return writeMidiOutputPattern(d, pattern, channel, midiOutputTarget(s, channel), notes, "write MIDI output to the piano roll");
}

}  // namespace gl
