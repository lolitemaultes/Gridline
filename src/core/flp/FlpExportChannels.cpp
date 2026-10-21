// Gridline — FLP export: channel rack (sampler, plugin, layer, audio-clip and
// automation channels) and plugin events (J-34).
//
// A channel is written as FL 25 writes it (event order of the shipped FL
// 25.2 projects): opener 64, type 21, plugin events (201 name, 212 wrapper,
// 203 display name, 155 icon, 128 colour, 41, 213 state), switch 0, echo 209,
// 138, 139, time shift 89, swing mix 97, 48, precomputed effects 69 86 71 83
// 74, fades 75 76, 85, 131, flags 70, insert 104, layer children 94, 50,
// levels 219 + 229, polyphony 221, parameters 215, cut groups 132, layer
// flags 144, display group 145, automation points 234, 32, key tracking 228 ×2,
// envelopes 218 ×5, 143, ping-pong 20, sample 196, root note 135, 170, 51.
//
// Values come from the Gridline channel; settings Gridline does not model
// come from the channel's FL origin events (when it was imported) or FL's
// defaults. The channel's derived values (levels, tuning, sampler settings,
// envelopes, LFO, …) are solved through the importer itself: candidate
// events are imported as a one-channel project and compared with the
// Gridline channel, so export and import cannot drift apart.
#include <algorithm>
#include <cmath>
#include <limits>
#include "../Automation.h"
#include "../AutomationEdit.h"
#include "../PluginParams.h"
#include "../../engine/ChannelIns.h"
#include "FlpExportInternal.h"

namespace gl::flp::xp {

namespace {

using enc::Type;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ------------------------------------------------------------ FL defaults

Bytes words(std::initializer_list<int64_t> v) {
  ByteWriter w;
  for (int64_t x : v) w.u32(static_cast<uint32_t>(x));
  return w.b;
}

Bytes defaultWrapper(int kind, int slot) {  // 212: plugin window state (positions left at 0)
  // kind 0: sampler-type channel, 1: plugin channel, 2: effect slot
  if (kind == 0) return words({0, 0, -1, 0, 0x50, 3, 0, 0, 0, 0, 0, 0, 0});
  if (kind == 1) return words({0, 0, 2, 0, 0x150, 0, 0, 0, 0, 0, 0, 0, 0});
  return words({0, slot, 2, 0, 0x140, 0, 0, 0, 0, 0, 0, 0, 0});
}

Bytes defaultEcho(int ppq) { return words({0, 6400, 0, 4, ppq * 3 / 2}); }  // feedback, pan, pitch, echoes, time
Bytes defaultLevels() { return words({6400, 10000, 0, 256, 0, 0}); }       // pan, volume, pitch, mod X, mod Y, filter type
Bytes defaultLevelAdjust() { return words({0, 12800, 0, 0, 0}); }          // pan offset, volume ×, …
Bytes defaultPoly() {  // max voices, porta time, flags
  ByteWriter w;
  w.u32(0);
  w.u32(500);
  w.u8(0);
  return w.b;
}

// 215: channel parameters (sampler settings, arpeggiator, time stretch).
// (FL 24 / 25 values per channel type: J-39, from the shipped projects.)
Bytes defaultParams(int type) {
  const bool audio = type == kAudio, plain = type == kSampler || type == kPlugin;
  ByteWriter w;
  w.i32(audio ? 0x8C : -1);
  w.u32(0);
  w.u8(1);                // @8
  w.u8(0);                // @9 remove DC
  w.u8(0);                // @10 flags (2 = ping-pong echo)
  w.u8(plain ? 1 : 0);    // @11
  w.i32(-1);              // @12
  w.u32(60);              // @16
  for (int i = 0; i < 5; ++i) w.f32(1.0f);  // @20..39
  w.u32(0);     // @40 arpeggiator direction
  w.u32(audio ? 3 : 1);   // @44 range
  w.i32(audio ? 0 : -1);  // @48 chord
  w.u32(1024);  // @52 time (1024 = one step)
  w.u32(48);    // @56 gate
  w.u8(0);
  w.u8(0);
  w.u8(1);
  w.u8(0);      // @60
  w.u32(1447);  // @64
  w.u32(0);     // @68
  w.u32(256);   // @72
  w.u32(0);     // @76
  w.u8(0);      // @80 normalize
  w.u8(0);      // @81 polarity
  w.u8(0);      // @82
  w.u8(audio ? 1 : 0);  // @83 declick
  w.u32(0);     // @84 loop crossfade
  w.u32(0);     // @88
  w.u32(1);     // @92 arpeggiator repeat
  w.u32(0);     // @96 stretch to time
  w.i32(0);     // @100 pitch shift (cents)
  w.i32(0);     // @104 time multiplier (log2 × 10000)
  w.u32(0);     // @108 stretch mode
  w.u32(2);     // @112
  w.i32(-2);    // @116
  w.i32(-1);    // @120
  w.u32(0);     // @124
  w.f64(0.0);   // @128 start
  w.f64(1.0);   // @136 end
  w.u32(0);
  w.u32(0);
  w.i32(-1);
  w.u32(0x101);  // @156 (FL 24 / 25)
  w.f64(0.5);   // @160
  return w.b;
}

Bytes defaultKeyTrack(int first) { return words({first, 0, 0, 0}); }

// 218: envelope / LFO (index 1 = volume carries flag 4 and -101 at @64).
Bytes defaultEnvelope(int index) {
  return words({index == 1 ? 4 : 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, index == 1 ? -101 : 0});
}

// 234: automation points header + records + trailer.
Bytes automationTrailer() {
  ByteWriter w;
  w.u32(1);
  for (int i = 0; i < 4; ++i) w.i32(-1);
  w.u32(128);
  w.u32(128);
  w.u32(0);
  w.u32(128);
  w.u32(5);
  w.u32(3);
  w.u32(1);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.f64(1.0);
  w.u32(0);
  w.u32(0);
  w.u32(1);
  w.u32(0);
  for (int i = 0; i < 3; ++i) w.i32(-1);
  w.u32(0xB2FB);
  for (int i = 0; i < 4; ++i) w.u32(0);
  return w.b;
}

// ------------------------------------------------------------ helpers

struct ChanEvents {
  EventList ev;
  RawEvent* get(uint8_t id, int nth = 0) {
    int k = 0;
    for (auto& e : ev)
      if (e.id == id && k++ == nth) return &e;
    return nullptr;
  }
  const RawEvent* get(uint8_t id, int nth = 0) const {
    int k = 0;
    for (const auto& e : ev)
      if (e.id == id && k++ == nth) return &e;
    return nullptr;
  }
  void setValue(uint8_t id, uint32_t v) {
    if (RawEvent* e = get(id)) {
      for (size_t i = 0; i < e->data.size() && i < 4; ++i) e->data[i] = static_cast<uint8_t>(v >> (8 * i));
    }
  }
  uint32_t value(uint8_t id, uint32_t dflt) const {
    const RawEvent* e = get(id);
    return e ? e->value() : dflt;
  }
};

// Canonical FL 25 channel event list (sizes and order), filled with FL's
// defaults, then with whatever the template has for the same ids.
EventList canonicalChannel(int type, int ppq, const EventList& tmpl) {
  EventList out;
  auto add = [&](uint8_t id, Bytes b) {
    RawEvent e{id, std::move(b)};
    out.push_back(std::move(e));
  };
  auto scalar = [&](uint8_t id, uint32_t v) {
    Bytes b;
    for (int i = 0; i < eventWidth(id); ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    add(id, b);
  };
  scalar(21, static_cast<uint32_t>(type));
  add(201, utf16z(""));
  add(212, defaultWrapper(type == kPlugin ? 1 : 0, 0));
  add(203, utf16z(""));
  scalar(155, 0);
  scalar(128, 0x5A5A5A);
  scalar(41, 1);  // FL 24 / 25: 1 on most channels
  if (type == kPlugin) add(213, {});
  scalar(0, 1);
  add(209, defaultEcho(ppq));
  scalar(138, 0x00800080);
  scalar(139, 0x00010000);
  scalar(89, 0);
  scalar(97, 128);
  scalar(48, 0);
  scalar(69, 128);
  scalar(86, 256);
  scalar(71, 1024);
  scalar(83, 0);
  scalar(74, 0);
  scalar(75, 0);
  scalar(76, 0);
  scalar(85, 2048);
  scalar(131, 0x00800000);
  scalar(70, 0);
  scalar(104, 0);
  scalar(50, 1);
  add(219, type == kAuto ? words({0, 12800, 0, 256, 0, 0}) : defaultLevels());  // automation channels: pan 0, full volume (FL 24 / 25)
  add(229, defaultLevelAdjust());
  {
    Bytes poly = defaultPoly();
    if (type == kAuto) poly[0] = 1;  // automation channels: 1 (FL 24 / 25)
    add(221, poly);
  }
  add(215, defaultParams(type));
  if (type != kLayer) scalar(132, 0);
  scalar(144, 0);
  scalar(145, 0);
  if (type == kAuto) add(234, {});
  scalar(32, 0);
  add(228, defaultKeyTrack(100));
  add(228, defaultKeyTrack(60));
  for (int i = 0; i < 5; ++i) add(218, defaultEnvelope(i));
  scalar(143, type == kAudio ? 2 : type == kAuto ? 3 : 10);
  scalar(20, 0);
  scalar(170, type == kPlugin || type == kAudio ? 0xFFFFFFFFu : 0);
  scalar(51, 0);
  // Template values for the same ids (in order of occurrence), and template
  // events this layout does not list (kept, after 51).
  std::map<uint8_t, int> seen;
  std::vector<const RawEvent*> extra;
  for (const auto& t : tmpl) {
    if (t.id == 64 || t.id == 22 || t.id == 142 || t.id == 196 || t.id == 135 || t.id == 94 || t.id == 213 || t.id == 201 || t.id == 234)
      continue;  // written from the model
    int nth = seen[t.id]++;
    int k = 0;
    bool placed = false;
    for (auto& e : out)
      if (e.id == t.id && k++ == nth) {
        if (eventWidth(t.id) == 0 || t.data.size() == e.data.size()) e.data = t.data;
        placed = true;
        break;
      }
    if (!placed && t.id != 72) extra.push_back(&t);  // 72: legacy volume word (superseded by 219)
  }
  for (const RawEvent* e : extra) out.push_back(*e);
  return out;
}

// One-channel project for the importer (the oracle the solver decodes with).
Bytes miniProject(const EventList& ch, const Exporter& x) {
  EventWriter w;
  w.ascii(199, x.o.version);
  w.dword(156, static_cast<uint32_t>(std::lround(x.s.tempo * 1000.0)));
  w.word(64, 0);
  for (const auto& e : ch) w.raw(e);
  return w.file(0, 1, static_cast<uint16_t>(x.s.ppq));
}

bool decodeChannel(const EventList& ch, const Exporter& x, Channel& out) {
  Song song;
  Report rep;
  std::string err;
  if (!importFlpBytes(miniProject(ch, x), song, rep, &err, x.ec.factoryData, x.ec.projectDir, false)) return false;
  if (song.channels.empty()) return false;
  out = song.channels[0];
  return true;
}

// A channel value the solver matches, and where it is stored.
struct ChanField {
  std::function<double(const Channel&)> get;
  uint8_t id = 0;
  int nth = 0;          // occurrence of the event (218 ×5, 228 ×2)
  size_t offset = 0;
  Type type = Type::I32;
  double lo = 0, hi = 0;
};

double readRaw(const ChanEvents& ce, const ChanField& f) {
  const RawEvent* e = ce.get(f.id, f.nth);
  return e ? enc::readField(e->data, f.offset, f.type) : kNaN;
}
void writeRaw(ChanEvents& ce, const ChanField& f, double v) {
  if (RawEvent* e = ce.get(f.id, f.nth)) enc::writeField(e->data, f.offset, f.type, v);
}

double sp(const Channel& c, int k) {
  return static_cast<size_t>(k) < c.plugin.params.size() ? c.plugin.params[static_cast<size_t>(k)] : kNaN;
}

// Solves one field: the stored value whose import gives the target's value.
bool solveField(ChanEvents& ce, const ChanField& f, const Channel& target, const Exporter& x, int& decodes) {
  const double want = f.get(target);
  if (!std::isfinite(want)) return false;
  auto valueAt = [&](double r) {
    writeRaw(ce, f, r);
    Channel c;
    ++decodes;
    return decodeChannel(ce.ev, x, c) ? f.get(c) : kNaN;
  };
  const double r0 = readRaw(ce, f);
  if (!std::isfinite(r0)) return false;
  double cur = valueAt(r0);
  if (static_cast<float>(cur) == static_cast<float>(want)) return true;
  double best = r0, bestErr = std::isfinite(cur) ? std::fabs(cur - want) : HUGE_VAL;
  bool hit = false;
  auto consider = [&](double r, double v) {
    if (!std::isfinite(v)) return;
    double err = std::fabs(v - want);
    if (err < bestErr || (err == bestErr && std::fabs(r - r0) < std::fabs(best - r0))) best = r, bestErr = err;
    hit = hit || static_cast<float>(v) == static_cast<float>(want);
  };
  const bool flt = f.type == Type::F32 || f.type == Type::F64;
  // Stored floats are mostly the value itself (sample start / end): try it
  // first — the search below needs distinct values at the range ends, which
  // laws with a default outside the range (end = 1 when not in 0..1) lack.
  if (flt && want >= f.lo && want <= f.hi) consider(want, valueAt(want));
  if (hit) {
  } else if (!flt && f.hi - f.lo <= 300) {
    for (double r = f.lo; r <= f.hi && !hit; r += 1) consider(r, valueAt(r));
  } else {
    double a = f.lo, z = f.hi, va = valueAt(a), vz = valueAt(z);
    consider(a, va);
    consider(z, vz);
    if (std::isfinite(va) && std::isfinite(vz) && va != vz) {
      const bool inc = vz > va;
      for (int it = 0; it < 200 && !hit; ++it) {
        double m = flt ? 0.5 * (a + z) : std::floor(0.5 * (a + z));
        if ((flt && (m == a || m == z)) || (!flt && z - a <= 1)) break;
        double v = valueAt(m);
        if (!std::isfinite(v)) break;
        consider(m, v);
        if ((v < want) == inc) a = m;
        else z = m;
      }
    }
  }
  writeRaw(ce, f, best);
  return hit;
}

// Fields of a channel the importer derives values from (see FlpChannels.cpp).
std::vector<ChanField> channelFields(int type, const Channel& target, int ppq) {
  using namespace gl::pp;
  std::vector<ChanField> f;
  auto add = [&](std::function<double(const Channel&)> g, uint8_t id, int nth, size_t off, Type t, double lo, double hi) {
    f.push_back(ChanField{std::move(g), id, nth, off, t, lo, hi});
  };
  auto spf = [](int k) { return [k](const Channel& c) { return sp(c, k); }; };
  if (type != kAuto) {
    add([](const Channel& c) { return c.pan; }, 219, 0, 0, Type::I32, 0, 12800);
    add([](const Channel& c) { return c.gain; }, 219, 0, 4, Type::U32, 0, 16000);
    if (type != kAudio) add([](const Channel& c) { return c.pitch * 100.0 + c.fine; }, 219, 0, 8, Type::I32, -2600, 2600);
    add([](const Channel& c) { return c.swingMix; }, 97, 0, 0, Type::U16, 0, 128);
    add([](const Channel& c) { return c.timeShift; }, 89, 0, 0, Type::U16, 0, 1024);
    add([](const Channel& c) { return c.polyphony; }, 221, 0, 0, Type::U32, 0, 256);
    add([](const Channel& c) { return c.portaTime; }, 221, 0, 4, Type::U32, 0, 2048);
    if (type != kLayer) {
      add([](const Channel& c) { return c.cutGroup; }, 132, 0, 0, Type::U16, 0, kMaxCutGroups);
      add([](const Channel& c) { return c.cutBy; }, 132, 0, 2, Type::U16, 0, kMaxCutGroups);
    }
    if (target.echo.active()) {
      add([](const Channel& c) { return c.echo.feedback; }, 209, 0, 0, Type::U32, 1, 12800);
      add([](const Channel& c) { return c.echo.pan; }, 209, 0, 4, Type::I32, 0, 12800);
      add([](const Channel& c) { return c.echo.pitch; }, 209, 0, 8, Type::I32, -1200, 1200);
      add([](const Channel& c) { return c.echo.count; }, 209, 0, 12, Type::U32, 1, 10);
      add([](const Channel& c) { return c.echo.time; }, 209, 0, 16, Type::U32, 0, 16.0 * ppq);  // 0 reads as the shortest time
      add([](const Channel& c) { return c.echo.modX; }, 138, 0, 0, Type::U16, 0, 256);
      add([](const Channel& c) { return c.echo.modY; }, 138, 0, 2, Type::U16, 0, 256);
    }
  }
  const bool sampler = target.plugin.type == "gl.sampler";
  if (sampler && type == kSampler) {
    add(spf(SmpRoot), 135, 0, 0, Type::U32, 0, 127);
    add(spf(SmpPreTilt), 69, 0, 0, Type::U16, 0, 256);
    add(spf(SmpPreCutoff), 71, 0, 0, Type::U16, 16, 1024);
    add(spf(SmpPreBoost), 74, 0, 0, Type::U16, 0, 256);
    add(spf(SmpPreReso), 83, 0, 0, Type::U16, 0, 640);
    add(spf(SmpPreStereoDelay), 85, 0, 0, Type::U16, 0, 4096);
    add(spf(SmpPreWarp), 86, 0, 0, Type::U16, 0, 512);
    add(spf(SmpPreRingMix), 131, 0, 0, Type::U16, 0, 256);
    add(spf(SmpPreRingFreq), 131, 0, 2, Type::U16, 0, 256);
    add(spf(SmpPreReverbMix), 139, 0, 0, Type::U16, 0, 256);
    add(spf(SmpPreReverbType), 139, 0, 2, Type::U16, 0, 1);
    add(spf(SmpPreRampIn), 76, 0, 0, Type::U16, 0, 1024);
    add(spf(SmpPreRampOut), 75, 0, 0, Type::U16, 0, 1024);
    add(spf(SmpLoop), 20, 0, 0, Type::U8, 0, 1);
    add(spf(SmpRemoveDc), 215, 0, 9, Type::U8, 0, 1);
    add(spf(SmpNormalize), 215, 0, 80, Type::U8, 0, 1);
    add(spf(SmpPolarity), 215, 0, 81, Type::U8, 0, 1);
    add(spf(SmpDeclick), 215, 0, 83, Type::U8, 0, 5);
    add(spf(SmpLoopXfade), 215, 0, 84, Type::U32, 0, 256);
    add(spf(SmpStart), 215, 0, 128, Type::F64, 0, 1);
    add(spf(SmpEnd), 215, 0, 136, Type::F64, 0, 1);
    add(spf(SmpTimeStretch), 215, 0, 104, Type::I32, -20000, 20000);
    add(spf(SmpPitchShift), 215, 0, 100, Type::I32, -1200, 1200);
    // Filter (levels 219: mod X / mod Y / type).
    add(spf(SmpFilterType), 219, 0, 20, Type::U32, 0, 7);
    add(spf(SmpCutoff), 219, 0, 12, Type::U32, 0, 256);
    add(spf(SmpReso), 219, 0, 16, Type::U32, 0, 256);
    // Volume envelope (218 #1).
    add(spf(SmpOneShot), 218, 1, 4, Type::I32, -1, 0);  // FL stores "on" as -1
    add(spf(SmpEnvDelay), 218, 1, 8, Type::I32, 100, 65536);
    add(spf(SmpAttack), 218, 1, 12, Type::I32, 100, 65536);
    add(spf(SmpEnvHold), 218, 1, 16, Type::I32, 100, 65536);
    add(spf(SmpDecay), 218, 1, 20, Type::I32, 100, 65536);
    add(spf(SmpSustain), 218, 1, 24, Type::I32, 0, 128);
    add(spf(SmpRelease), 218, 1, 28, Type::I32, 100, 65536);
    // Cutoff envelope (218 #2).
    add(spf(SmpFiltEnv), 218, 2, 32, Type::I32, -128, 128);
    add(spf(SmpFiltAttack), 218, 2, 12, Type::I32, 100, 65536);
    add(spf(SmpFiltDecay), 218, 2, 20, Type::I32, 100, 65536);
    add(spf(SmpFiltSustain), 218, 2, 24, Type::I32, 0, 128);
    add(spf(SmpFiltRelease), 218, 2, 28, Type::I32, 100, 65536);
    // LFO: on the envelope of its target (volume #1, pan #0, cutoff #2, pitch #4).
    static const int kEnvOf[5] = {-1, 1, 0, 2, 4};
    const int t = static_cast<int>(std::lround(sp(target, SmpLfoTarget)));
    if (t >= 1 && t <= 4) {
      const int e = kEnvOf[t];
      add(spf(SmpLfoAmount), 218, e, 44, Type::I32, -128, 128);
      add(spf(SmpLfoShape), 218, e, 52, Type::I32, 0, 2);
      add(spf(SmpLfoRate), 218, e, 48, Type::I32, 200, 65536);
      add(spf(SmpLfoDelay), 218, e, 36, Type::I32, 100, 65536);
    }
  } else if (sampler && type == kAudio) {
    add(spf(SmpStretchMode), 215, 0, 108, Type::U32, 0, 1);
    add(spf(SmpTimeStretch), 215, 0, 104, Type::I32, -20000, 20000);
    add(spf(SmpPitchShift), 215, 0, 100, Type::I32, -1200, 1200);
  } else if (target.plugin.type == "gl.synth" && target.plugin.ins.empty()) {
    // Trio Synth: FL's channel volume envelope is its amplitude envelope
    // (unless the channel carries its own envelopes, written whole below).
    add([](const Channel& c) { return sp(c, SynAttack); }, 218, 1, 12, Type::I32, 100, 65536);
    add([](const Channel& c) { return sp(c, SynDecay); }, 218, 1, 20, Type::I32, 100, 65536);
    add([](const Channel& c) { return sp(c, SynSustain); }, 218, 1, 24, Type::I32, 0, 128);
    add([](const Channel& c) { return sp(c, SynRelease); }, 218, 1, 28, Type::I32, 100, 65536);
  }
  return f;
}

}  // namespace


int Exporter::channelType(const Channel& c) const {
  if (audioChannels.count(c.id)) return kAudio;
  const EventList orig = origin(originKey("ch", c.id));
  int ot = -1;
  if (const RawEvent* e = findRaw(orig, 21)) ot = static_cast<int>(e->value());
  if (c.plugin.type == "gl.sampler") return ot == kAudio ? kAudio : kSampler;
  // An instrument-less channel is FL's layer channel (also without children:
  // that is what imports as one, J-39); an origin sampler stays one.
  if (c.plugin.empty()) return ot == kSampler && !c.isLayer() ? kSampler : kLayer;
  if (!hasFlPlugin(c.plugin, c.id, -1)) return kSampler;  // placeholder: an empty sampler channel
  return kPlugin;
}

namespace {

// Points of an automation clip as FL's event 234 (format 3: 24-byte records:
// f64 beats after the previous point, f64 value, f32 tension, u8 curve mode,
// 2 bytes, u8 tension sign 255 / 1 / 0), with the template's header word,
// trailer and the original numbers where they still decode to the point.
Bytes automationPoints(const AutomationClip& a, Exporter& x, const RawEvent* old) {
  Bytes trailer = automationTrailer();
  uint8_t hdrFlag = 0;
  uint32_t hdrB = 64;
  std::vector<Bytes> oldRecs;
  if (old && old->data.size() >= 21 && getU32(old->data, 13) >= 2) {
    const uint32_t n = getU32(old->data, 17);
    hdrB = getU32(old->data, 4);
    hdrFlag = old->data[8];
    const size_t end = 21 + static_cast<size_t>(n) * 24;
    if (end <= old->data.size()) {
      trailer.assign(old->data.begin() + static_cast<std::ptrdiff_t>(end), old->data.end());
      for (uint32_t i = 0; i < n; ++i)
        oldRecs.emplace_back(old->data.begin() + 21 + static_cast<std::ptrdiff_t>(i) * 24, old->data.begin() + 21 + static_cast<std::ptrdiff_t>(i + 1) * 24);
    }
  }
  uint32_t target = 0;
  const bool hasTarget = x.flTarget(ParamId{a.owner, a.index}, target);
  const int ppq = std::max(1, x.s.ppq);
  ByteWriter b;
  b.u32(1);
  b.u32(hdrB);
  b.u8(hdrFlag);
  b.u32(4);
  b.u32(3);
  b.u32(static_cast<uint32_t>(a.points.size()));
  Tick prev = 0;
  for (size_t i = 0; i < a.points.size(); ++i) {
    const AutoPoint& p = a.points[i];
    Bytes r(24, 0);
    const bool haveOld = i < oldRecs.size();
    if (haveOld) r = oldRecs[i];
    const Tick d = p.pos - prev;
    prev = p.pos;
    if (!(haveOld && std::llround(getF64(r, 0) * ppq) == d)) putF64(r, 0, static_cast<double>(d) / ppq);
    const double norm = a.min + static_cast<double>(p.value) * (a.max - a.min);
    bool keep = false;
    if (haveOld && hasTarget) {
      const double g = x.glValue(target, getF64(r, 8));
      keep = std::isfinite(g) && static_cast<float>(g) == static_cast<float>(norm);
    }
    if (!keep) {
      // Targets without a value law keep the clip value as is (the importer
      // sets the clip range to the target's), else the law is inverted.
      double v = static_cast<double>(p.value);
      if (hasTarget) {
        const double g = x.glValue(target, v);
        if (!(std::isfinite(g) && static_cast<float>(g) == static_cast<float>(norm) && a.min == static_cast<float>(x.glValue(target, 0.0)) &&
              a.max == static_cast<float>(x.glValue(target, 1.0))))
          v = x.flValue(target, norm);
      }
      if (!std::isfinite(v)) v = p.value;
      putF64(r, 8, v);
    }
    if (!(haveOld && autoedit::tensionFromFl(getF32(r, 16)) == p.tension)) putF32(r, 16, -p.tension);  // FL's sign is opposite
    r[20] = static_cast<uint8_t>(autoedit::flModeOf(p.shape));
    const float t = getF32(r, 16);
    r[23] = t < 0 ? 255 : t > 0 ? 1 : (haveOld && r[23] != 255 && r[23] != 1 ? r[23] : 0);
    b.bytes(r);
  }
  b.bytes(trailer);
  return b.b;
}

}  // namespace

void Exporter::writeChannel(int fl) {
  const Chan& slot = chans[static_cast<size_t>(fl)];
  w.word(64, static_cast<uint32_t>(fl));
  if (slot.automation) {
    const AutomationClip* a = s.automation(slot.id);
    const EventList orig = origin(originKey("auto", slot.id));
    EventList tmpl;
    for (const auto& e : stripPseudo(orig))
      if (e.id != 227 && e.id != 64) tmpl.push_back(e);
    if (a && unchanged(orig, fingerprintAutomation(*a, im))) {
      for (const auto& e : tmpl) w.raw(e);
      rep.counts["automation channels (verbatim)"]++;
      return;
    }
    ChanEvents ce{canonicalChannel(kAuto, s.ppq, tmpl)};
    if (a) {
      if (RawEvent* e = ce.get(203)) e->data = utf16z(a->name);
      ce.setValue(128, flColor(a->color));
      if (RawEvent* e = ce.get(234)) e->data = automationPoints(*a, *this, findRaw(tmpl, 234));
      if (a->loop > 0) rep.warn("automation '" + a->name + "': looped content is written once (FL loops automation clips by their length)");
    }
    for (const auto& e : ce.ev) w.raw(e);
    rep.counts["automation channels"]++;
    return;
  }

  const Channel* chp = s.channel(slot.id);
  if (!chp) return;
  const Channel& ch = *chp;
  const int type = channelType(ch);
  const EventList orig = origin(originKey("ch", ch.id));
  EventList tmpl;
  for (const auto& e : stripPseudo(orig))
    if (e.id != 64) tmpl.push_back(e);
  int origType = -1;
  if (const RawEvent* e = findRaw(tmpl, 21)) origType = static_cast<int>(e->value());
  const bool placeholder = type == kSampler && !ch.plugin.empty() && ch.plugin.type != "gl.sampler";
  PluginOut po;
  if (type == kPlugin) po = encodePlugin(ch.plugin, ch.id, -1, true, "channel '" + ch.name + "'");
  if (placeholder)
    reportPlugin({"channel '" + ch.name + "'", ch.plugin.type, "", "placeholder", "no FL instrument; written as an empty sampler channel named after it"},
                 ch.id, -1);
  else if (ch.plugin.type == "gl.sampler" && standIns && standIns->find(ch.id, -1))  // J-80: rendered to a sampler channel
    reportPlugin({"channel '" + ch.name + "'", ch.plugin.type, "Sampler", "rendered", ""}, ch.id, -1);
  // Replaces the plugin events (201 212 213) of a list with the encoded plugin.
  auto putPlugin = [&](EventList& ev) {
    if (type != kPlugin) return;
    EventList out;
    bool done = false;
    for (const auto& e : ev) {
      const bool group = e.id == 201 || e.id == 212 || e.id == 213;
      if (!group) {
        out.push_back(e);
        continue;
      }
      if (e.id == 201 && !done) {
        out.push_back(RawEvent{201, utf16z(po.flName)});
        out.push_back(RawEvent{212, po.wrapper});
        done = true;
      }
      if (e.id == 213 && po.hasState) out.push_back(RawEvent{213, po.state});
    }
    if (po.hasState && !findRaw(out, 213)) {  // after 41 (FL's order)
      auto it = std::find_if(out.begin(), out.end(), [](const RawEvent& e) { return e.id == 41; });
      out.insert(it == out.end() ? out.end() : it + 1, RawEvent{213, po.state});
    }
    ev = std::move(out);
  };

  if (origType == type && unchanged(orig, fingerprintChannel(ch, im))) {
    EventList ev = tmpl;
    putPlugin(ev);
    for (const auto& e : ev) w.raw(e);
    if (type == kAudio)
      if (const RawEvent* e = findRaw(ev, 219); e && e->data.size() >= 12) audioCents[ch.id] = static_cast<int32_t>(getU32(e->data, 8));
    rep.counts["channels (verbatim)"]++;
    return;
  }

  // From the model, on the template.
  ChanEvents ce{canonicalChannel(type, s.ppq, origType == type ? tmpl : EventList{})};
  if (RawEvent* e = ce.get(203)) e->data = utf16z(placeholder ? "[Gridline] " + ch.name : ch.name);
  ce.setValue(128, flColor(ch.color));
  ce.setValue(0, ch.muted ? 0 : 1);
  ce.setValue(155, static_cast<uint32_t>(ch.flIcon));  // J-39
  {
    const int ins = im.ins(ch.mixerTrack);
    ce.setValue(104, static_cast<uint32_t>(static_cast<uint16_t>(ins < 0 ? 0 : ins)));
  }
  ce.setValue(145, static_cast<uint32_t>(ch.group >= 0 ? ch.group : 0));
  // J-43: an audio-clip channel's pitch (219 @8, cents) is what its clips
  // carry (the importer adds it to Clip::pitch; FL's clip record has no pitch
  // field). Written when the channel's clips agree on it.
  if (type == kAudio)
    if (RawEvent* e = ce.get(219); e && e->data.size() >= 12) {
      bool seen = false, same = true;
      float p = 0;
      auto scan = [&](const std::vector<Clip>& clips) {
        for (const Clip& c : clips)
          if (c.kind == ClipKind::Audio && c.source == ch.id) {
            if (!seen) p = c.pitch, seen = true;
            else same = same && c.pitch == p;
          }
      };
      scan(s.playlist);
      for (const auto& a : s.arrangements) scan(a.playlist);
      if (seen && same && std::fabs(p) <= 1e6f) putU32(e->data, 8, static_cast<uint32_t>(static_cast<int32_t>(std::lround(p))));
    }
  if (type == kLayer) {
    ce.setValue(144, (ce.value(144, 0) & ~1u) | (ch.layerMode == 1 ? 1u : 0u));
    auto it = std::find_if(ce.ev.begin(), ce.ev.end(), [](const RawEvent& e) { return e.id == 104; });
    size_t at = it == ce.ev.end() ? ce.ev.size() : static_cast<size_t>(it - ce.ev.begin()) + 1;
    for (ObjectId k : ch.layer) {
      const int idx = im.ch(k);
      if (idx < 0) continue;
      ce.ev.insert(ce.ev.begin() + static_cast<std::ptrdiff_t>(at++), RawEvent{94, {static_cast<uint8_t>(idx), static_cast<uint8_t>(idx >> 8)}});
    }
  }
  if (RawEvent* e = ce.get(215); e && e->data.size() > 10) {
    if (ch.echo.active() && ch.echo.pingPong) e->data[10] = static_cast<uint8_t>(e->data[10] | 2);
    else e->data[10] = static_cast<uint8_t>(e->data[10] & ~2);
  }
  if (RawEvent* e = ce.get(221); e && e->data.size() > 8)
    e->data[8] = static_cast<uint8_t>((e->data[8] & ~3) | (ch.mono ? 1 : 0) | (ch.porta ? 2 : 0));
  if (!ch.echo.active())
    if (RawEvent* e = ce.get(209); e && e->data.size() >= 4) {
      Channel probe;
      if (decodeChannel(ce.ev, *this, probe) && probe.echo.active()) putU32(e->data, 0, 0);
    }
  // Arpeggiator (gl.arp) → 215 @40 direction, @44 range, @52 time, @56 gate, @92 repeat.
  if (RawEvent* e = ce.get(215); e && e->data.size() >= 96) {
    const PluginState* arp = nullptr;
    for (const auto& n : ch.noteFx)
      if (n.type == "gl.arp" && !arp) arp = &n;
    if (!arp) {
      putU32(e->data, 40, 0);
    } else {
      auto prm = [&](int k) { return static_cast<size_t>(k) < arp->params.size() ? arp->params[static_cast<size_t>(k)] : 0.0f; };
      static const int kDir[5] = {1, 2, 3, 3, 5};
      static const int kBack[6] = {0, 0, 1, 2, 2, 4};
      const int dir = std::clamp(static_cast<int>(std::lround(prm(0))), 0, 4);
      const uint32_t oldDir = getU32(e->data, 40);
      if (!(oldDir >= 1 && oldDir <= 5 && kBack[oldDir] == dir)) putU32(e->data, 40, static_cast<uint32_t>(kDir[dir]));
      putU32(e->data, 44, static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(prm(1))) + 1, 1, 4)));
      static const double kRateBeats[12] = {4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
      const int rate = std::clamp(static_cast<int>(std::lround(prm(2))), 0, 11);
      {
        const double beats = getU32(e->data, 52) / 1024.0 * 0.25;
        int best = 4;
        for (int k = 0; k < 12; ++k)
          if (std::fabs(std::log2(kRateBeats[k] / std::max(1e-3, beats))) < std::fabs(std::log2(kRateBeats[best] / std::max(1e-3, beats)))) best = k;
        if (best != rate) putU32(e->data, 52, static_cast<uint32_t>(std::lround(kRateBeats[rate] / 0.25 * 1024.0)));
      }
      const double gate = std::clamp(getU32(e->data, 56) / 100.0, 0.05, 1.0);
      if (static_cast<float>(gate) != prm(3)) putU32(e->data, 56, static_cast<uint32_t>(std::lround(prm(3) * 100.0)));
      putU32(e->data, 92, static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(prm(4))), 1, 8)));
    }
    if (ch.noteFx.size() > (arp ? 1u : 0u)) rep.warn("channel '" + ch.name + "': note processors other than one arpeggiator have no FL channel equivalent");
  }
  if (ch.plugin.type == "gl.sampler") {
    using namespace gl::pp;
    uint32_t fx = ce.value(70, 0);
    auto bit = [&](uint32_t b, int k) { fx = sp(ch, k) >= 0.5 ? (fx | b) : (fx & ~b); };
    if (type == kSampler) {
      bit(1, SmpPreFadeStereo);
      bit(4, SmpPreClip);
      bit(256, SmpSwapStereo);
    }
    bit(2, SmpReverse);
    ce.setValue(70, fx);
    // Time stretch (215 @100 pitch shift, @104 time multiplier, @108 mode),
    // set together: the importer only reads them as a group.
    if (type == kSampler)
      if (RawEvent* e = ce.get(215); e && e->data.size() >= 112) {
        Channel probe;
        const bool ok = decodeChannel(ce.ev, *this, probe) && probe.plugin.type == "gl.sampler";
        const bool same = ok && sp(probe, SmpStretchMode) == sp(ch, SmpStretchMode) && sp(probe, SmpTimeStretch) == sp(ch, SmpTimeStretch) &&
                          sp(probe, SmpPitchShift) == sp(ch, SmpPitchShift);
        if (!same) {
          if (sp(ch, SmpStretchMode) >= 0.5) {
            if (getU32(e->data, 108) == 0) putU32(e->data, 108, 1);
            putI32(e->data, 104, static_cast<int32_t>(std::lround(std::log2(std::max(1e-6, sp(ch, SmpTimeStretch))) * 10000.0)));
            putI32(e->data, 100, static_cast<int32_t>(std::lround(sp(ch, SmpPitchShift))));
          } else {
            putU32(e->data, 108, 0);
            putI32(e->data, 104, 0);
            putI32(e->data, 100, 0);
          }
        }
      }
    auto after20 = [&] {
      auto it = std::find_if(ce.ev.begin(), ce.ev.end(), [](const RawEvent& e) { return e.id == 20; });
      return it == ce.ev.end() ? ce.ev.end() : it + 1;
    };
    // Sample path (196), then root note (135), after the ping-pong flag (20).
    const RawEvent* oldRoot = findRaw(tmpl, 135);
    if (type == kSampler && (sp(ch, SmpRoot) != 60 || oldRoot)) ce.ev.insert(after20(), RawEvent{135, oldRoot ? oldRoot->data : words({60})});
    if (!ch.plugin.data.empty()) {
      const RawEvent* oldPath = findRaw(tmpl, 196);
      Bytes path = utf16z(enc::flPath(ch.plugin.data, ec));
      if (oldPath) {  // the original text when it still resolves to the same file
        Song tmp;
        Report r;
        File f;
        Context c(f, tmp, r);
        c.factoryData = ec.factoryData;
        c.projectDir = ec.projectDir;
        if (c.resolvePath(oldPath->view().text()) == ch.plugin.data) path = oldPath->data;
      }
      ce.ev.insert(after20(), RawEvent{196, path});
    }
  }
  // Plugins that play the channel's sample (Slicex, Granulizer, …): the
  // channel keeps its path (196), the decoders read it from there.
  if (type == kPlugin && !ch.plugin.data.empty() && !findRaw(ce.ev, 196))
    if (const PluginInfo* info = findPlugin(ch.plugin.type); info && info->usesSample && ch.plugin.data.find('\n') == std::string::npos) {
      auto it = std::find_if(ce.ev.begin(), ce.ev.end(), [](const RawEvent& e) { return e.id == 20; });
      ce.ev.insert(it == ce.ev.end() ? ce.ev.end() : it + 1, RawEvent{196, utf16z(enc::flPath(ch.plugin.data, ec))});
    }
  putPlugin(ce.ev);
  // 3x Osc (v14+): FL plays the channel's own envelopes (218) and filter
  // levels (219), not the copies inside the state (5 × 68-byte records at 96,
  // same layout; levels copy 0, 0, cutoff, resonance, 0, type at 436): carry
  // the encoded copies over so FL hears what the state says.
  if (type == kPlugin && po.hasState && ch.plugin.type == "gl.synth" && po.flName == "3x Osc" && po.state.size() >= 460) {
    for (int t = 0; t < 5; ++t)
      if (RawEvent* e = ce.get(218, t); e && e->data.size() >= 68)
        std::copy_n(po.state.begin() + 96 + 68 * t, 68, e->data.begin());
    if (RawEvent* e = ce.get(219); e && e->data.size() >= 24) {
      putU32(e->data, 12, getU32(po.state, 444));
      putU32(e->data, 16, getU32(po.state, 448));
      putU32(e->data, 20, getU32(po.state, 456));
    }
  }
  // Sampler cutoff envelope (Mod X, 218 #2): the importer reads it only when
  // the envelope is on with an amount, so switch it on (FL's -1) for the
  // solver to set the amount and times (J-39).
  if (type == kSampler && ch.plugin.type == "gl.sampler" && sp(ch, gl::pp::SmpFiltEnv) != 0)
    if (RawEvent* e = ce.get(218, 2); e && e->data.size() >= 36) {
      if (getU32(e->data, 4) == 0) putI32(e->data, 4, -1);
      if (getU32(e->data, 32) == 0) putI32(e->data, 32, sp(ch, gl::pp::SmpFiltEnv) > 0 ? 64 : -64);
    }
  // Echo delay (209): the importer reads it only with a feedback above 0, and
  // the inactive echo's feedback reads as the default (0.5) — switch it on
  // first so the solver sees every field (J-39).
  if (type != kAuto && ch.echo.active())
    if (RawEvent* e = ce.get(209); e && e->data.size() >= 20 && getU32(e->data, 0) == 0) putU32(e->data, 0, 6400);
  // Derived values, solved through the importer.
  int decodes = 0;
  const std::vector<ChanField> fields = channelFields(type, ch, s.ppq);
  for (int pass = 0; pass < 2; ++pass) {
    Channel cur;
    if (!decodeChannel(ce.ev, *this, cur)) break;
    bool any = false;
    for (const ChanField& f : fields) {
      if (!ce.get(f.id, f.nth)) continue;
      const double want = f.get(ch), have = f.get(cur);
      if (!std::isfinite(want) || static_cast<float>(want) == static_cast<float>(have)) continue;
      solveField(ce, f, ch, *this, decodes);
      any = true;
    }
    if (!any) break;
  }
  // J-72: a plugin channel's own envelopes / LFOs / filter (PluginState::ins)
  // are FL's channel records, field for field: 218 × 5 (pan, volume, mod X,
  // mod Y, pitch), the filter words of 219 (cutoff, resonance, type) and the
  // velocity / key tracking 228 × 2.
  if (type == kPlugin && !ch.plugin.ins.empty()) {
    InsSettings st;
    parseIns(ch.plugin.ins, st);
    for (int t = 0; t < InsTargets; ++t)
      if (RawEvent* e = ce.get(218, t); e && e->data.size() >= 68) {
        const InsTarget& x = st.t[t];
        const int32_t v[17] = {x.flags,    x.on ? -1 : 0, x.predelay,  x.attack,   x.hold,     x.decay,   x.sustain,  x.release, x.amount,
                               x.lfoDelay, x.lfoAttack,   x.lfoAmount, x.lfoSpeed, x.lfoShape, x.tensionA, x.tensionD, x.tensionR};
        for (int k = 0; k < 17; ++k) putI32(e->data, 4 * static_cast<size_t>(k), v[k]);
      }
    if (RawEvent* e = ce.get(219); e && e->data.size() >= 24) {
      putI32(e->data, 12, st.cutoff);
      putI32(e->data, 16, st.reso);
      putI32(e->data, 20, st.type);
    }
    for (int k = 0; k < 2; ++k)  // velocity / key tracking
      if (RawEvent* e = ce.get(228, k); e && e->data.size() >= 16)
        for (int j = 0; j < 4; ++j) putI32(e->data, 4 * static_cast<size_t>(j), st.track[k][j]);
  }
  // Trio Synth decay = the channel volume envelope's decay + hold (as the
  // importer reads them): the decay knob alone cannot reach every sum, so the
  // hold is searched with it (J-39).
  if (type == kPlugin && ch.plugin.type == "gl.synth" && ch.plugin.ins.empty())
    if (RawEvent* e = ce.get(218, 1); e && e->data.size() >= 32 && getU32(e->data, 4) != 0) {
      Channel cur;
      const float want = static_cast<float>(sp(ch, gl::pp::SynDecay));
      if (decodeChannel(ce.ev, *this, cur) && static_cast<float>(sp(cur, gl::pp::SynDecay)) != want) {
        auto law = [](int32_t h, int32_t d) { return static_cast<float>(std::clamp(flEnvTime(d) + flEnvTime(h), 0.005, 10.0)); };
        int32_t fh = 0, fd = 0;
        auto tryHold = [&](int32_t h) {
          int32_t a = 100, z = 65536;
          if (law(h, a) > want || law(h, z) < want) return false;
          while (z - a > 1) {
            const int32_t m = a + (z - a) / 2;
            if (law(h, m) < want) a = m;
            else z = m;
          }
          for (int32_t d : {a, z})
            if (law(h, d) == want) {
              fh = h, fd = d;
              return true;
            }
          return false;
        };
        bool found = tryHold(static_cast<int32_t>(getU32(e->data, 16))) || tryHold(100);
        for (int32_t h = 101; !found && h <= 65536; ++h) found = tryHold(h);
        if (found) {
          const Bytes keep = e->data;
          putI32(e->data, 16, fh), putI32(e->data, 20, fd);
          Channel chk;
          if (!(decodeChannel(ce.ev, *this, chk) && static_cast<float>(sp(chk, gl::pp::SynDecay)) == want)) e->data = keep;
        }
      }
    }
  rep.counts["channel solver imports"] += decodes;
  // Volume between the 219 steps: the 229 multiplier makes up the rest
  // (gain = volume / 12800 × multiplier / 12800, as the importer reads it).
  if (type != kAuto)
    if (RawEvent* lv = ce.get(219); lv && lv->data.size() >= 8)
      if (RawEvent* adj = ce.get(229); adj && adj->data.size() >= 8) {
        Channel cur;
        if (decodeChannel(ce.ev, *this, cur) && cur.gain != ch.gain) {
          auto gainOf = [](uint32_t v, uint32_t m) {
            const float g = static_cast<float>(std::clamp(v / 12800.0, 0.0, 1.25));
            return static_cast<float>(std::clamp(g * (m / 12800.0), 0.0, 1.25));
          };
          for (uint32_t v = 1; v <= 16000; ++v) {
            const double m0 = ch.gain * 12800.0 / std::clamp(v / 12800.0, 0.0, 1.25);
            bool found = false;
            for (int d = -1; d <= 1 && !found; ++d) {
              const double m = std::round(m0) + d;
              if (m < 0 || m > 65535) continue;
              if (gainOf(v, static_cast<uint32_t>(m)) == ch.gain) {
                const Bytes l0 = lv->data, a0 = adj->data;
                putU32(lv->data, 4, v);
                putU32(adj->data, 4, static_cast<uint32_t>(m));
                Channel chk;
                if (decodeChannel(ce.ev, *this, chk) && chk.gain == ch.gain) found = true;
                else lv->data = l0, adj->data = a0;
              }
            }
            if (found) break;
          }
        }
      }
  {
    Channel back;
    if (decodeChannel(ce.ev, *this, back)) {
      std::vector<std::string> diff;
      auto cmp = [&](const char* n, double a, double b) {
        if (static_cast<float>(a) != static_cast<float>(b)) diff.push_back(n);
      };
      cmp("volume", back.gain, ch.gain);
      cmp("pan", back.pan, ch.pan);
      if (type != kAudio) cmp("pitch", back.pitch * 100.0 + back.fine, ch.pitch * 100.0 + ch.fine);
      if (s.swing != 0) cmp("swing mix", back.swingMix, ch.swingMix);
      cmp("time shift", back.timeShift, ch.timeShift);
      cmp("polyphony", back.polyphony, ch.polyphony);
      // J-78 / J-80: settings only where they play a part (porta time: porta / mono / gliding notes; swing mix: song swing).
      bool glides = ch.porta || ch.mono;
      for (const Pattern& p : s.patterns)
        if (auto it = p.notes.find(ch.id); it != p.notes.end() && !glides)
          for (const Note& n : it->second) glides = glides || n.porta || n.slide;
      if (glides) cmp("porta time", back.portaTime, ch.portaTime);
      if (!(back.echo == ch.echo) && (back.echo.active() || ch.echo.active())) diff.push_back("echo");
      if (ch.plugin.type == "gl.sampler" && back.plugin.type == "gl.sampler") {
        const PluginInfo* info = findPlugin("gl.sampler");
        for (size_t k = 0; k < ch.plugin.params.size() && k < back.plugin.params.size(); ++k)
          if (back.plugin.params[k] != ch.plugin.params[k]) diff.push_back(info && k < info->params.size() ? info->params[k].name : "#" + std::to_string(k));
      }
      if (!diff.empty()) {
        std::string t;
        for (const auto& d : diff) t += (t.empty() ? "" : ", ") + d;
        rep.warn("channel '" + ch.name + "': not exact in FL: " + t);
        rep.counts["channels approximated"]++;
      }
    }
  }
  for (const auto& e : ce.ev) w.raw(e);
  if (type == kAudio)
    if (const RawEvent* e = ce.get(219); e && e->data.size() >= 12) audioCents[ch.id] = static_cast<int32_t>(getU32(e->data, 8));
  rep.counts["channels"]++;
}

}  // namespace gl::flp::xp
