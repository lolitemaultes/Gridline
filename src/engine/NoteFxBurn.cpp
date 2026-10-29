// Gridline — offline note processor rendering (see NoteFxBurn.h).
#include "NoteFxBurn.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include "../core/MenuOps.h"
#include "NoteFx.h"

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
  std::vector<Note> in;
  if (auto it = p->notes.find(channel); it != p->notes.end()) in = it->second;
  // Steps play too: each becomes a one-step note.
  if (auto it = p->steps.find(channel); it != p->steps.end()) {
    const Tick step = std::max<Tick>(1, s.ticksPerStep(*p));
    const auto propsIt = p->stepProps.find(channel);
    for (size_t i = 0; i < it->second.size(); ++i) {
      if (!it->second[i]) continue;
      Note n(static_cast<Tick>(i) * step, step, 60, std::min(127, static_cast<int>(it->second[i])));
      if (propsIt != p->stepProps.end() && i < propsIt->second.size() && propsIt->second[i].pitch >= 0)
        n.pitch = propsIt->second[i].pitch;
      in.push_back(n);
    }
  }
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

}  // namespace gl
