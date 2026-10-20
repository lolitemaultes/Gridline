// Gridline — FLP export driver: index assignment, project header, plugin
// encoding, automation targets, trailer, report, round trip (J-34).
//
// File layout written (FL 25.2, as the shipped FL 25 projects):
//   project header (199 version … 216)
//   per pattern with notes: 65 index, 224 notes, 223 controller events
//   226 MIDI remote links, 227 automation / controller links
//   channel 0, pattern metadata (65, 193, 150, 157, 158, 164, …), channels 1…
//   arrangements (99, 241, 36, 233 playlist, markers, 238/43/239 tracks)
//   242 ×arrangements, 100 current arrangement, 29 39 40 31 38, 103 insert count
//   inserts (149 42 95 204 236, slots: plugin events + 98, 235 165 166 49 154 147)
//   225 mixer parameters, 133, 243, 47
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include "../Arrangements.h"
#include "../Params.h"
#include "FlpExportInternal.h"

namespace gl::flp {

namespace xp {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bytes words(std::initializer_list<int64_t> v) {
  ByteWriter w;
  for (int64_t x : v) w.u32(static_cast<uint32_t>(x));
  return w.b;
}

// J-45: FL plugins number their send targets and side inputs by the hosting
// insert's route list (flRouteOutputs / flRouteInputs). A Gridline state that
// names mixer tracks is turned into that numbering before it is encoded
// (a target the track does not route to becomes "none").
PluginState routeIndexed(const Song& s, int track, const PluginState& p) {
  const PluginInfo* pi = findPlugin(p.type);
  if (!pi || track < 0) return p;
  PluginState q = p;
  auto at = [&](int k) -> float& { return q.params[static_cast<size_t>(k)]; };
  auto has = [&](int k) { return k >= 0 && static_cast<size_t>(k) < q.params.size(); };
  if (pi->routeModeParam >= 0 && has(pi->routeModeParam) && at(pi->routeModeParam) < 0.5f) {
    const std::vector<int> outs = flRouteOutputs(s, track);
    std::vector<int> params = pi->sendParams;
    if (p.type == "gl.send") params.push_back(0);
    for (int k : params) {
      if (!has(k)) continue;
      const int to = static_cast<int>(std::lround(at(k)));
      int n = 0;
      for (size_t r = 1; r < outs.size() && to >= 0; ++r)
        if (outs[r] == to) n = static_cast<int>(r);
      at(k) = static_cast<float>(n);
    }
    at(pi->routeModeParam) = 1.0f;
  }
  if (has(pi->sideInParam) && at(pi->sideInParam) < 0) {  // the track's sidechain: its first sidechain-only source
    const std::vector<int> ins = flRouteInputs(s, track);
    int n = 0;
    for (size_t r = 1; r < ins.size() && n == 0; ++r)
      for (const Send& snd : s.mixer[static_cast<size_t>(ins[r])].sends)
        if (snd.sidechain && s.mixerIndex(snd.to) == track) n = static_cast<int>(r);
    at(pi->sideInParam) = static_cast<float>(n);
  }
  return q;
}
}  // namespace

// ------------------------------------------------------------ indices

void Exporter::assignIndices() {
  forEachArrangementClip(s, [&](const Clip& c) {
    if (c.kind == ClipKind::Audio && s.channel(c.source)) audioChannels.insert(c.source);
  });
  for (const Channel& c : s.channels)
    if (c.audioClip && c.plugin.type == "gl.sampler") audioChannels.insert(c.id);  // J-39: clip sources without clips
  // Channels + automation clips, ordered by their FL index when the rack
  // order still agrees with it (an unchanged import keeps FL's numbering).
  struct Item {
    Chan c;
    double key;
  };
  std::vector<Item> items;
  bool rackOk = true;
  int lastOrig = -1;
  std::vector<int> orig(s.channels.size(), -1);
  for (size_t i = 0; i < s.channels.size(); ++i) {
    orig[i] = originIndex(origin(originKey("ch", s.channels[i].id)));
    if (orig[i] >= 0) {
      if (orig[i] <= lastOrig) rackOk = false;
      lastOrig = orig[i];
    }
  }
  double prev = -1;
  for (size_t i = 0; i < s.channels.size(); ++i) {
    double key;
    if (!rackOk) key = static_cast<double>(i);
    else if (orig[i] >= 0) key = orig[i];
    else key = prev + 1e-3;
    prev = key;
    items.push_back({{false, s.channels[i].id}, key});
  }
  for (size_t i = 0; i < s.automations.size(); ++i) {
    const int o = originIndex(origin(originKey("auto", s.automations[i].id)));
    const double key = rackOk && o >= 0 ? o : 1e7 + (o >= 0 ? o : 1e6 + static_cast<double>(i));
    items.push_back({{true, s.automations[i].id}, key});
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
  for (size_t k = 0; k < items.size(); ++k) {
    chans.push_back(items[k].c);
    (items[k].c.automation ? im.automation : im.channel)[items[k].c.id] = static_cast<int>(k);
  }
  // Patterns: FL indices kept while they still increase in list order.
  bool patOk = true;
  int last = 0;
  std::vector<int> po;
  for (const auto& p : s.patterns) {
    const int o = originIndex(origin(originKey("pat", p.id)));
    po.push_back(o);
    if (o <= last) patOk = false;
    else last = o;
  }
  int next = 1;
  for (size_t i = 0; i < s.patterns.size(); ++i) {
    const int idx = patOk ? po[i] : next;
    next = idx + 1;
    patterns.push_back({idx, s.patterns[i].id});
    im.pattern[s.patterns[i].id] = idx;
  }
  for (size_t i = 0; i < s.mixer.size(); ++i) im.insert[s.mixer[i].id] = static_cast<int>(i);
  // Inserts written: the original count (FL 25: event 103; older FL: 127),
  // else FL's default mixer (master + 16 + the current one).
  int count = std::max<int>(static_cast<int>(s.mixer.size()) + 1, 18);
  if (o.useOrigin && !s.flOrigin.empty()) {
    const EventList cur = origin("mxcur");
    const int ci = originIndex(cur);
    if (ci >= 0) count = std::max<int>(static_cast<int>(s.mixer.size()) + 1, ci + 1);
  }
  insertCount = count;
}

// ------------------------------------------------------------ plugins

bool Exporter::hasFlPlugin(const PluginState& p, ObjectId owner, int slot) const {
  if (p.empty()) return false;
  const EventList orig = origin(pluginKey(owner, slot));
  if (!orig.empty() && originTypeMatches(orig, p.type)) return true;
  // Encoded already: what the encoder made of it (J-39: FL plugins that are
  // no native home of the type are written only when they are exact).
  if (auto it = pluginCache.find({owner, slot}); it != pluginCache.end()) return it->second.flPlugin;
  for (const auto& e : enc::encoders()) {
    if (!(e.glType == p.type || (e.glType == "*hosted" && isHostedType(p.type)))) continue;
    if (e.accepts && !e.accepts(p)) continue;
    if (e.nativeRank >= 0 || e.defaults) return true;
  }
  return false;
}

Exporter::PluginOut Exporter::encodePlugin(const PluginState& p, ObjectId owner, int slot, bool generator, const std::string& where) {
  const auto key = std::make_pair(owner, slot);
  if (auto it = pluginCache.find(key); it != pluginCache.end()) return it->second;
  PluginOut out;
  const EventList orig = stripPseudo(origin(pluginKey(owner, slot)));
  const bool fromOrigin = !orig.empty() && originTypeMatches(origin(pluginKey(owner, slot)), p.type);
  std::string oName;
  Bytes oState, oWrapper;
  bool oHasState = false;
  if (fromOrigin)
    for (const auto& e : orig) {
      if (e.id == 201) oName = e.view().text();
      if (e.id == 213) oState = e.data, oHasState = true;
      if (e.id == 212) oWrapper = e.data;
      if (e.id == 203 || e.id == 155 || e.id == 128 || e.id == 41) out.extras.push_back(e);
    }
  const auto host = slot >= 0 ? im.insert.find(owner) : im.insert.end();
  const PluginState q = host != im.insert.end() ? routeIndexed(s, host->second, p) : p;
  enc::Result r = enc::encode(q, oName, oHasState ? &oState : nullptr, ec);
  ExportReport::Plugin rp{where, p.type, r.flName, enc::qualityName(r.quality), r.note};
  if (r.quality == enc::Result::None || r.flName.empty()) {
    out.flPlugin = false;
    if (!generator) {  // effect placeholder: a neutral FL effect named after the Gridline plugin
      out.flPlugin = true;
      out.flName = "Fruity Balance";
      out.hasState = true;
      out.state = words({0, 256});
      const PluginInfo* info = findPlugin(p.type);
      out.extras = {RawEvent{203, utf16z("[Gridline] " + (info ? info->name : p.type))}};
      rp.flName = out.flName;
      rp.note = "no FL plugin; neutral placeholder (Fruity Balance at unity) named after it";
    }
    rp.quality = "placeholder";
    rep.counts["plugins placeholder"]++;
    if (!p.empty()) rep.warn(where + ": " + p.type + " has no FL equivalent (placeholder)");
  } else {
    out.flPlugin = true;
    out.flName = r.flName;
    out.hasState = r.hasState;
    out.state = r.state;
    rep.counts[std::string("plugins ") + enc::qualityName(r.quality)]++;
    if (r.quality == enc::Result::Approx) {
      const PluginInfo* info = findPlugin(p.type);
      std::string t;
      for (int k : r.mismatched) {
        if (t.size() > 160) {
          t += ", …";
          break;
        }
        t += (t.empty() ? "" : ", ") + (info && k >= 0 && static_cast<size_t>(k) < info->params.size() ? info->params[static_cast<size_t>(k)].name : "#" + std::to_string(k));
      }
      if (r.dataMismatch) t += (t.empty() ? "" : ", ") + std::string("data");
      rp.note = (rp.note.empty() ? "" : rp.note + "; ") + "not exact: " + t;
    }
    if (r.quality == enc::Result::Defaults) rp.note = (rp.note.empty() ? "" : rp.note + "; ") + "written without settings (FL loads its defaults)";
  }
  out.wrapper = !oWrapper.empty() && out.flName == oName ? oWrapper
                : generator ? words({0, 0, 2, 0, 0x150, 0, 0, 0, 0, 0, 0, 0, 0})
                            : words({0, slot, 2, 0, 0x140, 0, 0, 0, 0, 0, 0, 0, 0});
  rep.plugins.push_back(rp);
  if (out.flPlugin) {
    PluginMapping m;
    if (enc::decodeState(out.flName, out.hasState ? out.state : Bytes{}, m, ec)) writtenPlugins[key] = std::move(m);
  }
  pluginCache[key] = out;
  return out;
}

void Exporter::encodeAllPlugins() {
  for (const auto& c : s.channels) {
    if (c.plugin.empty() || c.plugin.type == "gl.sampler") continue;
    if (channelType(c) == kPlugin) encodePlugin(c.plugin, c.id, -1, true, "channel '" + c.name + "'");
  }
  for (size_t i = 0; i < s.mixer.size(); ++i)
    for (size_t k = 0; k < s.mixer[i].slots.size() && k < static_cast<size_t>(kEffectSlots); ++k)
      if (!s.mixer[i].slots[k].plugin.empty())
        encodePlugin(s.mixer[i].slots[k].plugin, s.mixer[i].id, static_cast<int>(k), false,
                     "insert " + std::to_string(i) + " slot " + std::to_string(k + 1));
}

// ------------------------------------------------------------ targets

struct Exporter::Resolver {
  File file;
  Song song;
  Report report;
  std::unique_ptr<Context> c;
};

Exporter::Resolver& Exporter::res() {
  if (resolver) return *resolver;
  resolver = std::make_shared<Resolver>();
  Resolver& r = *resolver;
  r.song = s;
  r.c = std::make_unique<Context>(r.file, r.song, r.report);
  Context& c = *r.c;
  for (const auto& [id, fl] : im.channel) c.channelIds[fl] = id;
  for (const auto& [id, fl] : im.insert) c.insertIds[fl] = id;
  for (const auto& [id, fl] : im.pattern) c.patternIds[fl] = id;
  for (const auto& [id, fl] : im.automation) c.automationIds[fl] = id;
  for (const auto& [key, m] : writtenPlugins) {
    if (key.second < 0) {
      if (int fl = im.ch(key.first); fl >= 0) c.channelPlugins[fl] = m;
    } else if (int ins = im.ins(key.first); ins >= 0) {
      c.slotPlugins[{ins, key.second}] = m;
    }
  }
  return r;
}

bool Exporter::flTarget(ParamId id, uint32_t& target) {
  std::vector<uint32_t> cand;
  if (id.owner == kSongId) {
    if (id.index == static_cast<uint32_t>(SongParam::Tempo)) cand.push_back(0x40000005u);
  } else if (int ch = im.ch(id.owner); ch >= 0) {
    const uint32_t hi = static_cast<uint32_t>(ch) << 16;
    switch (id.index) {
      case static_cast<uint32_t>(ChannelParam::Volume): cand.push_back(hi | 0x00); break;
      case static_cast<uint32_t>(ChannelParam::Pan): cand.push_back(hi | 0x01); break;
      case static_cast<uint32_t>(ChannelParam::Pitch): cand.push_back(hi | 0x04); break;
      default:
        if (id.index >= kChannelPluginBase && id.index < kNoteFxBase) {
          auto m = writtenPlugins.find({id.owner, -1});
          if (m != writtenPlugins.end() && m->second.param)
            for (int f = 0; f < 8192; ++f)
              if (m->second.param(f) == static_cast<int>(id.index - kChannelPluginBase)) {
                cand.push_back(hi | (0x8000u + static_cast<uint32_t>(f)));
                break;
              }
        }
    }
  } else if (int ins = im.ins(id.owner); ins >= 0) {
    auto mixer = [&](int slot, uint32_t lo) { return (static_cast<uint32_t>((kMixerBase + ins) * 64 + slot) << 16) | lo; };
    const uint32_t ix = id.index;
    if (ix == static_cast<uint32_t>(MixerParam::Volume)) cand.push_back(mixer(0, 0x1FC0));
    else if (ix == static_cast<uint32_t>(MixerParam::Pan)) cand.push_back(mixer(0, 0x1FC1));
    else if (ix == static_cast<uint32_t>(MixerParam::StereoSep)) cand.push_back(mixer(0, 0x1FC2));
    else if (ix >= static_cast<uint32_t>(MixerParam::EqLowGain) && ix <= static_cast<uint32_t>(MixerParam::EqHighGain))
      cand.push_back(mixer(0, 0x1FD0 + ix - static_cast<uint32_t>(MixerParam::EqLowGain)));
    else if (ix >= static_cast<uint32_t>(MixerParam::EqLowFreq) && ix <= static_cast<uint32_t>(MixerParam::EqHighFreq))
      cand.push_back(mixer(0, 0x1FD8 + ix - static_cast<uint32_t>(MixerParam::EqLowFreq)));
    else if (ix >= static_cast<uint32_t>(MixerParam::EqLowQ) && ix <= static_cast<uint32_t>(MixerParam::EqHighQ))
      cand.push_back(mixer(0, 0x1FE0 + ix - static_cast<uint32_t>(MixerParam::EqLowQ)));
    else if (ix >= kSendBase && ix < kSlotBase) cand.push_back(mixer(0, 0x1F40 + (ix - kSendBase)));
    else if (ix >= kSlotBase) {
      const int slot = static_cast<int>((ix - kSlotBase) / kSlotStride), k = static_cast<int>((ix - kSlotBase) % kSlotStride);
      if (slot < kEffectSlots) {
        if (k == static_cast<int>(kSlotMix)) cand.push_back(mixer(slot, 0x1F01));
        else if (k == static_cast<int>(kSlotEnable)) cand.push_back(mixer(slot, 0x1F00));
        else if (auto m = writtenPlugins.find({id.owner, slot}); m != writtenPlugins.end() && m->second.param)
          for (int f = 0; f < 8192; ++f)
            if (m->second.param(f) == k) {
              cand.push_back(mixer(slot, 0x8000u + static_cast<uint32_t>(f)));
              break;
            }
      }
    }
  }
  for (uint32_t t : cand) {
    FlTarget ft;
    if (resolveFlTarget(*res().c, t, ft) && ft.id == id) {
      target = t;
      return true;
    }
  }
  return false;
}

double Exporter::glValue(uint32_t target, double v) {
  FlTarget t;
  if (!resolveFlTarget(*res().c, target, t)) return kNaN;
  return t.toNormalized(v);
}

double Exporter::flValue(uint32_t target, double norm) {
  FlTarget t;
  if (!resolveFlTarget(*res().c, target, t)) return kNaN;
  auto f = [&](double v) { return t.toNormalized(v); };
  const double a0 = f(0), a1 = f(1);
  if (!std::isfinite(a0) || !std::isfinite(a1)) return kNaN;
  if (static_cast<float>(a0) == static_cast<float>(norm)) return 0;
  if (static_cast<float>(a1) == static_cast<float>(norm)) return 1;
  double best = 0, bestErr = std::fabs(a0 - norm);
  if (std::fabs(a1 - norm) < bestErr) best = 1, bestErr = std::fabs(a1 - norm);
  if (a0 != a1) {  // monotonic laws: bisection
    const bool inc = a1 > a0;
    double lo = 0, hi = 1;
    for (int it = 0; it < 80; ++it) {
      const double m = 0.5 * (lo + hi), v = f(m);
      if (!std::isfinite(v)) break;
      if (std::fabs(v - norm) < bestErr) best = m, bestErr = std::fabs(v - norm);
      if (static_cast<float>(v) == static_cast<float>(norm)) return m;
      if ((v < norm) == inc) lo = m;
      else hi = m;
    }
  }
  if (bestErr > 1e-6)  // other shapes: scan
    for (int i = 0; i <= 1024; ++i) {
      const double v = f(i / 1024.0);
      if (std::isfinite(v) && std::fabs(v - norm) < bestErr) best = i / 1024.0, bestErr = std::fabs(v - norm);
    }
  return best;
}

// ------------------------------------------------------------ header / trailer

namespace {
const RawEvent* findIn(const EventList& ev, uint8_t id, int nth = 0) {
  int k = 0;
  for (const auto& e : ev)
    if (e.id == id && k++ == nth) return &e;
  return nullptr;
}
uint32_t buildOf(const std::string& v) {
  const size_t p = v.rfind('.');
  return p == std::string::npos ? 0 : static_cast<uint32_t>(std::strtoul(v.c_str() + p + 1, nullptr, 10));
}
}  // namespace

void Exporter::writeHeader() {
  const EventList proj = origin("proj");
  const EventList pe = stripPseudo(proj);
  const bool same = unchanged(proj, fingerprintProject(s));
  auto pv = [&](uint8_t id, uint32_t d) {
    const RawEvent* e = findIn(pe, id);
    return e ? e->value() : d;
  };
  auto raw = [&](uint8_t id, int nth = 0) { return findIn(pe, id, nth); };
  w.ascii(199, o.version);
  w.dword(159, buildOf(o.version));
  w.dword(169, pv(169, 7));
  if (const RawEvent* e = raw(172)) w.raw(*e);
  w.byte(37, pv(37, 1));
  w.dword(156, static_cast<uint32_t>(std::lround(s.tempo * 1000.0)));
  {
    uint32_t cur = pv(67, 1);
    bool ok = false;
    for (const auto& [idx, id] : patterns) ok = ok || static_cast<uint32_t>(idx) == cur;
    if (!ok) cur = patterns.empty() ? 1 : static_cast<uint32_t>(patterns.front().first);
    w.word(67, cur);
  }
  w.byte(9, pv(9, 0));
  w.word(11, static_cast<uint32_t>(std::clamp(std::lround(s.swing * 128.0), 0L, 128L)));
  w.word(80, static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(std::lround(s.masterPitch * 100.0)))));
  w.byte(17, static_cast<uint32_t>(s.timeSigNum));
  w.byte(18, static_cast<uint32_t>(s.timeSigDen));
  w.byte(35, pv(35, 1));
  w.byte(23, pv(23, 0));
  {  // J-43: per-project automatic crossfades (the original byte when it says the same)
    const uint32_t ox = pv(44, 1);
    w.byte(44, (ox != 0) == s.autoCrossfades ? ox : (s.autoCrossfades ? 1u : 0u));
  }
  w.byte(30, pv(30, 1));
  w.byte(10, pv(10, 0));
  auto text = [&](uint8_t id, const std::string& v) {
    if (same)
      if (const RawEvent* e = raw(id)) {
        w.raw(*e);
        return;
      }
    w.text(id, v);
  };
  text(194, s.info.title == "Imported project" && same ? "" : s.info.title);
  text(206, s.info.genre);
  text(207, s.info.author);
  w.dword(167, pv(167, 0));
  if (const RawEvent* e = raw(202)) w.raw(*e);
  else w.text(202, "");
  if (same && raw(198) && !raw(195)) {
    w.raw(*raw(198));
  } else {
    text(195, s.info.comments);
  }
  if (!s.info.url.empty() || (same && raw(197))) text(197, s.info.url);
  if (const RawEvent* e = raw(172, 1)) w.raw(*e);
  if (same && raw(237)) {
    w.raw(*raw(237));
  } else {
    ByteWriter b;
    b.f64(s.info.created > 0 ? static_cast<double>(s.info.created) / 86400.0 + 25569.0 : 0.0);
    b.f64(static_cast<double>(s.info.workTime) / 86400.0);
    w.data(237, b.b);
  }
  if (s.channelGroups.empty()) w.text(231, "Unsorted");
  for (size_t i = 0; i < s.channelGroups.size(); ++i) {
    const RawEvent* e = same ? raw(231, static_cast<int>(i)) : nullptr;
    if (e) w.raw(*e);
    else w.text(231, s.channelGroups[i]);
  }
  w.dword(146, pv(146, 0xFFFFFFFFu));
  if (const RawEvent* e = raw(216)) w.raw(*e);
  else w.data(216, {});
}

void Exporter::writeTrailer() {
  const EventList pe = stripPseudo(origin("proj"));
  auto pv = [&](uint8_t id, uint32_t d) {
    const RawEvent* e = findIn(pe, id);
    return e ? e->value() : d;
  };
  w.dword(133, pv(133, 1227));
  if (const RawEvent* e = findIn(pe, 243)) w.raw(*e);
  else w.data(243, words({-2, 0}));
  w.byte(47, pv(47, 0));
}

// ------------------------------------------------------------ links

void Exporter::writeLinks() {
  // 226 MIDI remote links: as imported while the links are unchanged, else
  // the Gridline links (plus the imported links to "the focused channel",
  // which Gridline does not keep).
  const EventList lo = origin("links");
  const EventList le = stripPseudo(lo);
  if (unchanged(lo, fingerprintLinks(s, im)) && !le.empty()) {
    for (const auto& e : le) w.raw(e);
  } else {
    for (const auto& e : le) {
      FlRemoteLink r;
      if (decodeRemoteLink(e.view(), r) && (r.focused() || !r.midi())) w.raw(e);
    }
    for (const auto& l : s.links) {
      if (l.action != RemoteAction::None) continue;
      uint32_t target = 0;
      if (!flTarget(ParamId{l.owner, l.index}, target)) {
        rep.counts["MIDI remote links without FL target"]++;
        continue;
      }
      int control = l.cc;
      switch (l.source) {
        case RemoteSource::CC: control = l.cc; break;
        case RemoteSource::ChannelPressure: control = 253; break;
        case RemoteSource::KeyPressure: control = 254; break;
        case RemoteSource::PitchBend: control = 255; break;
        case RemoteSource::Note: control = 256 + l.cc; break;
        default: control = -1; break;
      }
      if (control < 0) {
        rep.counts["MIDI remote links without FL source"]++;
        continue;
      }
      static const int kMode[4] = {0, 1, 2, 3};
      const uint32_t src = (static_cast<uint32_t>(l.port >= 0 ? l.port + 1 : 0) << 22) |
                           (static_cast<uint32_t>(std::max(0, l.midiChannel) & 0xF) << 16) | static_cast<uint32_t>(control);
      ByteWriter b;
      b.u32(src);
      b.u32(0);
      b.u32(target);
      b.u32(4u | (static_cast<uint32_t>(kMode[static_cast<int>(l.button) & 3]) << 8));
      b.u32(0x1D5);
      w.data(226, b.b);
      rep.counts["MIDI remote links"]++;
    }
  }
  // 227: automation clip targets (one per clip, in FL channel order).
  for (size_t fl = 0; fl < chans.size(); ++fl) {
    if (!chans[fl].automation) continue;
    const AutomationClip* a = s.automation(chans[fl].id);
    if (!a) continue;
    const EventList orig = origin(originKey("auto", a->id));
    std::vector<const RawEvent*> old;
    for (const auto& e : orig)
      if (e.id == 227) old.push_back(&e);
    uint32_t target = 0;
    const bool has = a->owner != kNoId && flTarget(ParamId{a->owner, a->index}, target);
    if (unchanged(orig, fingerprintAutomation(*a, im)) && !old.empty()) {
      for (const RawEvent* e : old) {  // the source channel index is rewritten
        RawEvent r = *e;
        putU16(r.data, 2, static_cast<uint32_t>(fl));
        w.raw(r);
      }
      continue;
    }
    if (!has) {
      if (a->owner != kNoId) rep.warn("automation '" + a->name + "': its target has no FL equivalent (written without a link)");
      continue;
    }
    Bytes b = old.empty() ? words({0, 0, 0, 8, 0x1D5}) : old.front()->data;
    if (b.size() < 12) b.resize(20, 0);
    putU16(b, 0, 0);
    putU16(b, 2, static_cast<uint32_t>(fl));
    putU32(b, 8, target);
    w.data(227, b);
  }
  // 227 with an output source: internal controller links.
  const EventList mo = origin("modlinks");
  if (unchanged(mo, fingerprintModLinks(s, im))) {
    for (const auto& e : stripPseudo(mo)) w.raw(e);
  } else {
    for (const auto& l : s.modLinks) {
      uint32_t target = 0;
      if (!flTarget(ParamId{l.owner, l.index}, target)) {
        rep.counts["controller links without FL target"]++;
        {
          auto m = writtenPlugins.find({l.owner, -1});
          std::string why = m == writtenPlugins.end() ? " (no written plugin)" : !m->second.param ? " (" + m->second.flName + ": no parameter map)" : " (" + m->second.flName + ")";
          rep.warn("controller link to " + paramDisplayName(s, ParamId{l.owner, l.index}) + ": no FL target" + why);
        }
        continue;
      }
      uint32_t src;
      if (l.sourceSlot < 0) {
        const int fl = im.ch(l.sourceOwner);
        if (fl < 0) continue;
        src = static_cast<uint32_t>(fl);
      } else {
        const int ins = im.ins(l.sourceOwner);
        if (ins < 0) continue;
        src = static_cast<uint32_t>((kMixerBase + ins) * 64 + l.sourceSlot);
      }
      ByteWriter b;
      b.u16(0x8000u | static_cast<uint32_t>(l.sourceOutput));
      b.u16(src);
      b.u32(0);
      b.u32(target);
      b.u32(8);
      b.u32(0x1D5);
      w.data(227, b.b);
      if (l.curve != ModCurve::Linear || l.min != 0 || l.max != 1 || l.smoothMs != 0)
        rep.counts["controller links: mapping curve / range not written"]++;
    }
  }
}

}  // namespace xp

// ------------------------------------------------------------ driver

bool exportFlp(const Song& song, Bytes& out, ExportReport& rep, const ExportOptions& opt) {
  xp::Exporter x(song, opt, rep);
  x.ec.tempo = song.tempo;
  x.ec.ppq = song.ppq;
  x.ec.factoryData = opt.factoryData;
  x.ec.projectDir = opt.projectDir;
  x.assignIndices();
  x.encodeAllPlugins();
  x.writeHeader();
  x.writePatternNotes();
  x.writeLinks();
  for (size_t fl = 0; fl < x.chans.size(); ++fl) {
    x.writeChannel(static_cast<int>(fl));
    if (fl == 0) x.writePatternMeta();
  }
  if (x.chans.empty()) x.writePatternMeta();
  x.writeArrangements();
  x.writeMixer();
  x.writeTrailer();
  int channels = 0;
  for (const auto& c : x.chans) channels += !c.automation;
  out = x.w.file(0, static_cast<uint16_t>(x.chans.size()), static_cast<uint16_t>(song.ppq));
  rep.counts["events"] = static_cast<int>(x.w.events().size());
  (void)channels;
  return true;
}

bool exportFlpFile(const Song& song, const std::string& path, ExportReport& rep, const ExportOptions& opt, std::string* err) {
  Bytes b;
  if (!exportFlp(song, b, rep, opt)) {
    if (err) *err = "export failed";
    return false;
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) {
      if (err) *err = "cannot write " + path;
      return false;
    }
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) {
      if (err) *err = "cannot write " + path;
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  return true;
}

std::string reportText(const ExportReport& rep) {
  std::ostringstream os;
  for (const auto& [k, v] : rep.counts) os << "  " << k << ": " << v << "\n";
  for (const auto& p : rep.plugins)
    if (p.quality != "verbatim" && p.quality != "exact")
      os << "  plugin " << p.where << ": " << p.glType << " -> " << (p.flName.empty() ? "-" : p.flName) << " [" << p.quality << "] " << p.note << "\n";
  for (const auto& wv : rep.warnings) os << "  warning: " << wv << "\n";
  return os.str();
}

}  // namespace gl::flp
