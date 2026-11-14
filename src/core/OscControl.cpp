// Gridline: the OSC address space (see OscControl.h and docs/osc.md).
#include "OscControl.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include "Document.h"
#include "Meter.h"

namespace gl {

namespace {

std::vector<std::string> segments(const std::string& a) {
  std::vector<std::string> v;
  std::string cur;
  for (size_t i = 1; i <= a.size(); ++i) {
    if (i == a.size() || a[i] == '/') {
      v.push_back(cur);
      cur.clear();
    } else {
      cur += a[i];
    }
  }
  return v;
}

bool kw(const std::string& seg, const char* word) { return osc::match(seg, word); }

// The numbers first .. first + count - 1 that a segment names (a number, or a pattern of numbers).
std::vector<int> numbers(const std::string& seg, int first, int count) {
  std::vector<int> v;
  if (seg.empty() || count <= 0) return v;
  if (!osc::isPattern(seg)) {
    if (seg.find_first_not_of("0123456789") != std::string::npos || seg.size() > 6) return v;
    const int n = std::atoi(seg.c_str());
    if (n >= first && n < first + count) v.push_back(n);
    return v;
  }
  for (int n = first; n < first + count && v.size() < 512; ++n)
    if (osc::match(seg, std::to_string(n))) v.push_back(n);
  return v;
}

struct Value {
  bool has = false;      // a number came with the message
  bool impulse = false;  // no value, or an impulse / nil: "do it"
  double v = 0;
};

Value valueAt(const osc::Message& m, size_t k) {
  Value r;
  if (k >= m.args.size()) {
    r.impulse = k == 0;
    return r;
  }
  const osc::Arg& a = m.args[k];
  if (a.type == 'I' || a.type == 'N') {
    r.impulse = true;
    return r;
  }
  if (a.numeric()) {
    r.has = true;
    r.v = a.number();
    if (!(r.v == r.v) || std::isinf(r.v)) r.has = false;  // NaN / infinity: no value
    return r;
  }
  if (a.text() && !a.s.empty()) {
    char* end = nullptr;
    const double d = std::strtod(a.s.c_str(), &end);
    if (end && *end == 0 && std::isfinite(d)) {
      r.has = true;
      r.v = d;
    }
  }
  return r;
}

bool pressed(const Value& v) { return v.impulse || (v.has && v.v >= 0.5); }

double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

std::string normName(const std::string& s) {
  std::string o;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

double stepOf(const ParamDesc& d) {
  if (d.kind == ParamKind::Toggle) return 1.0;
  if (d.kind == ParamKind::Choice && d.choices.size() > 1) return 1.0 / static_cast<double>(d.choices.size() - 1);
  return 0.0;
}

osc::Message msgF(const std::string& address, double v) {
  osc::Message m;
  m.address = address;
  m.args.push_back(osc::Arg::Float(static_cast<float>(v)));
  return m;
}

}  // namespace

const char* OscControl::resultName(Result r) {
  switch (r) {
    case Result::Learned: return "learned";
    case Result::Linked: return "linked";
    case Result::Handled: return "handled";
    case Result::Reserved: return "reserved";
    case Result::BadArgs: return "bad arguments";
    default: return "unknown address";
  }
}

double OscControl::paramNorm(ParamId pid) const {
  const Song& s = h_.doc().song();
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  return d && v ? d->toNormalized(*v) : -1.0;
}

const Expr* OscControl::formula(const std::string& text) {
  auto it = exprs_.find(text);
  if (it == exprs_.end()) {
    if (exprs_.size() > 128) exprs_.clear();
    Expr e;
    compileRemoteFormula(text, e);
    it = exprs_.emplace(text, std::move(e)).first;
  }
  return &it->second;
}

double OscControl::linkPosition(const ControllerLink& l, double target, const Expr* f) {
  if (l.encoder != RemoteEncoder::Absolute) return clamp01(target);
  auto g = [&](double u) { return std::fabs(remoteRange(l, remoteCurve(l, u, f)) - target); };
  double best = 0, bestErr = 1e9;
  for (int i = 0; i <= 128; ++i) {
    const double u = i / 128.0, e = g(u);
    if (e < bestErr) bestErr = e, best = u;
  }
  double lo = std::max(0.0, best - 1.0 / 128), hi = std::min(1.0, best + 1.0 / 128);
  for (int k = 0; k < 30; ++k) {
    const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
    if (g(a) <= g(b)) hi = b;
    else lo = a;
  }
  const double r = 0.5 * (lo + hi);
  return g(r) <= bestErr ? r : best;
}

void OscControl::watchParam(const std::string& address, ParamId pid, double norm) {
  auto it = watch_.find(address);
  if (it == watch_.end()) {
    if (watch_.size() >= kMaxWatched) return;
    it = watch_.emplace(address, Watch{}).first;
  }
  it->second.kind = Watch::Param;
  it->second.pid = pid;
  it->second.last = norm;
}

bool OscControl::setNorm(ParamId pid, const osc::Message& m, const std::string& address) {
  Document& d = h_.doc();
  auto desc = describeParam(d.song(), pid);
  auto cur = getParam(d.song(), pid);
  if (!desc || !cur) return false;
  const Value v = valueAt(m, 0);
  double n;
  if (desc->kind == ParamKind::Toggle && !v.has) n = *cur >= 0.5 ? 0.0 : 1.0;  // no value: toggles
  else if (!v.has) return false;
  else n = clamp01(v.v);
  d.setParam(pid, desc->fromNormalized(n), true);
  h_.edited();
  watchParam(address, pid, paramNorm(pid));
  return true;
}

// ------------------------------------------------------------------ dispatch ----

OscControl::Result OscControl::handle(const osc::Message& m) {
  if (!osc::validAddress(m.address)) return Result::Unknown;
  if (m.address == "/gridline/ping") {
    pong_ = true;
    return Result::Handled;
  }
  if (m.address.rfind("/gridline/", 0) == 0) return Result::Handled;  // the test button's own messages
  if (!osc::isPattern(m.address) && h_.learn(m.address)) return Result::Learned;
  if (links(m)) return Result::Linked;
  const auto seg = segments(m.address);
  if (seg.empty()) return Result::Unknown;
  if (kw(seg[0], "transport")) return transport(seg, m);
  if (kw(seg[0], "mixer")) return mixer(seg, m);
  if (kw(seg[0], "channel")) return channel(seg, m);
  if (kw(seg[0], "param")) return param(seg, m);
  if (kw(seg[0], "pattern")) return pattern(seg, m);
  if (kw(seg[0], "marker")) return marker(seg, m);
  if (kw(seg[0], "stage")) return Result::Reserved;  // kept for the Stage view (docs/osc.md)
  return Result::Unknown;
}

bool OscControl::links(const osc::Message& m) {
  std::vector<ControllerLink> hit;
  for (const auto& l : h_.doc().song().links)
    if (l.source == RemoteSource::Osc && !l.oscAddress.empty() &&
        (l.oscAddress == m.address || osc::match(m.address, l.oscAddress) || osc::match(l.oscAddress, m.address)))
      hit.push_back(l);
  if (hit.empty()) return false;
  const Value v = valueAt(m, 0);
  for (const auto& l : hit) {
    if (!v.has && !v.impulse) continue;  // a text or blob: nothing to map
    if (l.action != RemoteAction::None) {
      if (mapper_.triggerOsc(l, v.v, v.impulse)) h_.action(l.action);
      continue;
    }
    Document& d = h_.doc();
    const ParamId pid{l.owner, l.index};
    auto desc = describeParam(d.song(), pid);
    auto cur = getParam(d.song(), pid);
    if (!desc || !cur) continue;
    double out = 0;
    if (mapper_.processOsc(l, v.v, v.impulse, desc->toNormalized(*cur), &out, stepOf(*desc))) {
      d.setParam(pid, desc->fromNormalized(out), true);
      h_.edited();
      const Expr* f = l.curve == ModCurve::Formula ? formula(l.formula) : nullptr;
      linkSent_[RemoteMapper::key(l)] = linkPosition(l, paramNorm(pid), f);  // not echoed back
    }
  }
  return true;
}

OscControl::Result OscControl::transport(const std::vector<std::string>& seg, const osc::Message& m) {
  if (seg.size() < 2) return Result::Unknown;
  const std::string& w = seg[1];
  const Value v = valueAt(m, 0);
  Document& d = h_.doc();
  const Song& s = d.song();
  if (seg.size() == 2) {
    if (kw(w, "play")) {  // 1 / no value plays, 0 pauses
      if (v.has && v.v < 0.5) h_.pause();
      else h_.play();
      return Result::Handled;
    }
    if (kw(w, "stop")) {
      if (pressed(v)) h_.stop();
      return Result::Handled;
    }
    if (kw(w, "pause")) {
      if (pressed(v)) h_.pause();
      return Result::Handled;
    }
    if (kw(w, "playpause")) {
      if (pressed(v)) h_.playing() ? h_.pause() : h_.play();
      return Result::Handled;
    }
    if (kw(w, "record")) {
      h_.setRecording(v.has ? v.v >= 0.5 : !h_.recording());
      return Result::Handled;
    }
    if (kw(w, "metronome")) {
      if (pressed(v)) h_.action(RemoteAction::Metronome);
      return Result::Handled;
    }
    if (kw(w, "tempo")) {
      if (!v.has) return Result::BadArgs;
      d.setParam(songParam(SongParam::Tempo), std::clamp(v.v, 10.0, 522.0), true);
      h_.edited();
      return Result::Handled;
    }
    if (kw(w, "mode")) {
      if (!m.args.empty() && m.args[0].text()) {
        const std::string t = normName(m.args[0].s);
        if (t == "song") h_.setSongMode(true);
        else if (t == "pattern") h_.setSongMode(false);
        else return Result::BadArgs;
      } else if (v.has) {
        h_.setSongMode(v.v >= 0.5);
      } else {
        h_.setSongMode(!h_.songMode());
      }
      return Result::Handled;
    }
    if (kw(w, "loop")) {
      const Value b = valueAt(m, 1);
      if (!v.has || v.v <= 0 || !b.has) {  // no bars: the loop range is cleared
        h_.setLoop(0, 0);
        return Result::Handled;
      }
      const MeterMap mm(s);
      const int64_t first = static_cast<int64_t>(std::max(1.0, std::floor(v.v))) - 1;
      const int64_t last = static_cast<int64_t>(std::max(v.v, std::floor(b.v))) - 1;
      h_.setLoop(mm.barStart(first), mm.barStart(last + 1));
      return Result::Handled;
    }
    if (kw(w, "position")) {  // bars
      if (!v.has) return Result::BadArgs;
      const MeterMap mm(s);
      const double bar = std::max(1.0, v.v) - 1.0;
      const auto whole = static_cast<int64_t>(std::floor(std::min(bar, 1e6)));
      const Tick t0 = mm.barStart(whole);
      h_.setPosition(static_cast<double>(t0) + (bar - static_cast<double>(whole)) * static_cast<double>(mm.barLenAt(t0)));
      return Result::Handled;
    }
  }
  if (seg.size() == 3 && kw(w, "position")) {
    if (!v.has) return Result::BadArgs;
    if (kw(seg[2], "bars")) {
      osc::Message b = m;
      b.address = "/transport/position";
      return transport({"transport", "position"}, b);
    }
    if (kw(seg[2], "beats")) {
      h_.setPosition(std::clamp(v.v, 0.0, 1e7) * s.ppq);
      return Result::Handled;
    }
    if (kw(seg[2], "seconds")) {
      h_.setPosition(std::clamp(v.v, 0.0, 1e6) * s.tempo / 60.0 * s.ppq);
      return Result::Handled;
    }
  }
  if (seg.size() == 3 && kw(seg[2], "toggle")) {
    if (kw(w, "record")) {
      if (pressed(v)) h_.setRecording(!h_.recording());
      return Result::Handled;
    }
  }
  if (seg.size() == 3 && kw(w, "loop") && kw(seg[2], "off")) {
    h_.setLoop(0, 0);
    return Result::Handled;
  }
  return Result::Unknown;
}

OscControl::Result OscControl::mixer(const std::vector<std::string>& seg, const osc::Message& m) {
  if (seg.size() < 3) return Result::Unknown;
  Document& d = h_.doc();
  const int count = static_cast<int>(d.song().mixer.size());
  std::vector<int> idx = seg[1] == "master" ? std::vector<int>{0} : numbers(seg[1], 0, count);
  if (idx.empty()) return Result::Unknown;
  const std::string& w = seg[2];
  const Value v = valueAt(m, 0);
  Result r = Result::Unknown;
  for (int i : idx) {
    if (i >= static_cast<int>(d.song().mixer.size())) break;
    const ObjectId id = d.song().mixer[static_cast<size_t>(i)].id;
    const std::string base = "/mixer/" + std::to_string(i) + "/";
    if (seg.size() == 3) {
      MixerParam p = MixerParam::Count;
      if (kw(w, "volume")) p = MixerParam::Volume;
      else if (kw(w, "pan")) p = MixerParam::Pan;
      else if (kw(w, "stereo")) p = MixerParam::StereoSep;
      else if (kw(w, "mute")) p = MixerParam::Mute;
      if (p != MixerParam::Count) {
        const char* name = p == MixerParam::Volume ? "volume" : p == MixerParam::Pan ? "pan" : p == MixerParam::StereoSep ? "stereo" : "mute";
        r = setNorm(mixerParam(id, p), m, base + name) ? Result::Handled : Result::BadArgs;
        continue;
      }
      if (kw(w, "solo")) {
        const MixerTrack* t = d.song().mixerTrack(id);
        if (!t) continue;
        const bool on = v.has ? v.v >= 0.5 : !t->solo;
        if (on != t->solo) d.editMixerTrack(id, "solo mixer track", [on](MixerTrack& x) { x.solo = on; });
        h_.edited();
        auto it = watch_.find(base + "solo");
        if (it == watch_.end() && watch_.size() < kMaxWatched) it = watch_.emplace(base + "solo", Watch{}).first;
        if (it != watch_.end()) {
          it->second.kind = Watch::Solo;
          it->second.id = id;
          it->second.last = on ? 1 : 0;
        }
        r = Result::Handled;
        continue;
      }
    }
    if (seg.size() == 4 && kw(seg[3], "toggle") && (kw(w, "mute") || kw(w, "solo"))) {
      if (!pressed(v)) {
        r = Result::Handled;
        continue;
      }
      osc::Message t;
      t.address = base + (kw(w, "mute") ? "mute" : "solo");  // no value: toggles
      r = mixer(segments(t.address), t);
      continue;
    }
    if (seg.size() == 4 && kw(w, "send")) {
      for (int dest : numbers(seg[3], 0, count)) {
        if (dest == i || !v.has) continue;
        const ParamId pid = sendParam(id, dest);
        const std::string a = base + "send/" + std::to_string(dest);
        if (describeParam(d.song(), pid)) {
          r = setNorm(pid, m, a) ? Result::Handled : Result::BadArgs;
        } else {  // no send yet: one is made at this level
          if (d.setSend(id, d.song().mixer[static_cast<size_t>(dest)].id, static_cast<float>(clamp01(v.v) * 1.25))) {
            h_.edited();
            watchParam(a, pid, paramNorm(pid));
            r = Result::Handled;
          }
        }
      }
      continue;
    }
    if (seg.size() >= 5 && kw(w, "slot")) {
      const MixerTrack* t = d.song().mixerTrack(id);
      if (!t) continue;
      for (int sl : numbers(seg[3], 1, static_cast<int>(t->slots.size()))) {
        const std::string a = base + "slot/" + std::to_string(sl) + "/";
        if (seg.size() == 5 && kw(seg[4], "mix")) {
          r = setNorm(slotMixParam(id, sl - 1), m, a + "mix") ? Result::Handled : Result::BadArgs;
        } else if (seg.size() == 5 && kw(seg[4], "bypass")) {  // 1 bypasses (the slot's switch off), no value toggles
          const ParamId pid = slotEnableParam(id, sl - 1);
          auto cur = getParam(d.song(), pid);
          if (!cur) continue;
          const bool bypass = v.has ? v.v >= 0.5 : *cur >= 0.5;
          d.setParam(pid, bypass ? 0.0 : 1.0, true);
          h_.edited();
          watchParam(a + "enabled", pid, paramNorm(pid));
          r = Result::Handled;
        } else if (seg.size() == 5 && kw(seg[4], "enabled")) {
          r = setNorm(slotEnableParam(id, sl - 1), m, a + "enabled") ? Result::Handled : Result::BadArgs;
        }
      }
    }
  }
  return r;
}

OscControl::Result OscControl::channel(const std::vector<std::string>& seg, const osc::Message& m) {
  if (seg.size() < 3) return Result::Unknown;
  Document& d = h_.doc();
  const auto& chans = d.song().channels;
  std::vector<ObjectId> ids;
  std::vector<int> nums;
  if (seg[1] == "selected") {
    const ObjectId sel = h_.selectedChannel();
    for (size_t k = 0; k < chans.size(); ++k)
      if (chans[k].id == sel) ids.push_back(sel), nums.push_back(static_cast<int>(k) + 1);
  } else {
    for (int n : numbers(seg[1], 1, static_cast<int>(chans.size()))) ids.push_back(chans[static_cast<size_t>(n - 1)].id), nums.push_back(n);
  }
  if (ids.empty()) return Result::Unknown;
  const std::string& w = seg[2];
  Result r = Result::Unknown;
  for (size_t k = 0; k < ids.size(); ++k) {
    const ObjectId id = ids[k];
    const std::string base = "/channel/" + std::to_string(nums[k]) + "/";
    if (seg.size() == 3) {
      ChannelParam p = ChannelParam::Count;
      if (kw(w, "volume")) p = ChannelParam::Volume;
      else if (kw(w, "pan")) p = ChannelParam::Pan;
      else if (kw(w, "mute")) p = ChannelParam::Mute;
      else if (kw(w, "pitch")) p = ChannelParam::Pitch;
      if (p != ChannelParam::Count) {
        const char* name = p == ChannelParam::Volume ? "volume" : p == ChannelParam::Pan ? "pan" : p == ChannelParam::Mute ? "mute" : "pitch";
        r = setNorm(channelParam(id, p), m, base + name) ? Result::Handled : Result::BadArgs;
        continue;
      }
      if (kw(w, "note") || kw(w, "noteoff")) {
        const Value pv = valueAt(m, 0), vv = valueAt(m, 1);
        if (!pv.has) {
          r = Result::BadArgs;
          continue;
        }
        const int pitch = static_cast<int>(std::lround(std::clamp(pv.v, 0.0, 127.0)));
        int vel = 100;
        if (kw(w, "noteoff")) {
          vel = 0;
        } else if (vv.has) {
          const bool unit = (m.args[1].type == 'f' || m.args[1].type == 'd') && vv.v <= 1.0;
          vel = static_cast<int>(std::lround(std::clamp(unit ? vv.v * 127.0 : vv.v, 0.0, 127.0)));
        }
        h_.note(id, pitch, vel);
        if (vel > 0) held_.insert({id, pitch});
        else held_.erase({id, pitch});
        r = Result::Handled;
        continue;
      }
    }
    if (seg.size() == 4 && kw(w, "mute") && kw(seg[3], "toggle")) {
      if (pressed(valueAt(m, 0))) {
        osc::Message t;
        t.address = base + "mute";
        setNorm(channelParam(id, ChannelParam::Mute), t, t.address);
      }
      r = Result::Handled;
    }
  }
  return r;
}

OscControl::Result OscControl::param(const std::vector<std::string>& seg, const osc::Message& m) {
  // /param/mixer/<track>/<slot>/<param>  ·  /param/channel/<n>/<slot>/<param> (slot 0 = the instrument)
  if (seg.size() != 5) return Result::Unknown;
  Document& d = h_.doc();
  const Song& s = d.song();
  const bool isMixer = kw(seg[1], "mixer"), isChannel = !isMixer && kw(seg[1], "channel");
  if (!isMixer && !isChannel) return Result::Unknown;
  struct Target {
    ObjectId owner;
    int n, slot;
  };
  std::vector<Target> targets;
  if (isMixer) {
    for (int n : numbers(seg[2], 0, static_cast<int>(s.mixer.size())))
      for (int sl : numbers(seg[3], 1, static_cast<int>(s.mixer[static_cast<size_t>(n)].slots.size()))) targets.push_back({s.mixer[static_cast<size_t>(n)].id, n, sl});
  } else {
    const std::string slotSeg = seg[3] == "instrument" ? "0" : seg[3];
    for (int n : numbers(seg[2], 1, static_cast<int>(s.channels.size())))
      for (int sl : numbers(slotSeg, 0, kMaxNoteFx + 1)) targets.push_back({s.channels[static_cast<size_t>(n - 1)].id, n, sl});
  }
  if (targets.empty()) return Result::Unknown;
  Result r = Result::Unknown;
  for (const auto& t : targets) {
    auto pidOf = [&](int k) {
      if (isMixer) return slotParam(t.owner, t.slot - 1, k);
      return t.slot == 0 ? channelPluginParam(t.owner, k) : noteFxParam(t.owner, t.slot - 1, k);
    };
    const int limit = isMixer ? static_cast<int>(kSlotEnable) : t.slot == 0 ? static_cast<int>(kNoteFxBase - kChannelPluginBase) : static_cast<int>(kNoteFxStride);
    std::vector<int> ks;
    const std::string& p = seg[4];
    if (!p.empty() && p.find_first_not_of("0123456789") == std::string::npos) {
      const int k = p.size() <= 5 ? std::atoi(p.c_str()) : 0;
      if (k >= 1 && k <= limit) ks.push_back(k - 1);
    } else {  // by name (case, spaces and punctuation ignored), or a pattern of numbers or names
      const std::string want = normName(p);
      for (int k = 0; k < limit; ++k) {
        auto desc = describeParam(s, pidOf(k));
        if (!desc) break;
        if ((!want.empty() && normName(desc->name) == want) || (osc::isPattern(p) && (osc::match(p, std::to_string(k + 1)) || osc::match(p, desc->name))))
          ks.push_back(k);
      }
    }
    for (int k : ks) {
      const std::string a = "/param/" + std::string(isMixer ? "mixer/" : "channel/") + std::to_string(t.n) + "/" + std::to_string(t.slot) + "/" + std::to_string(k + 1);
      r = setNorm(pidOf(k), m, a) ? Result::Handled : Result::BadArgs;
    }
  }
  return r;
}

OscControl::Result OscControl::pattern(const std::vector<std::string>& seg, const osc::Message& m) {
  if (seg.size() != 2) return Result::Unknown;
  const Song& s = h_.doc().song();
  const int n = static_cast<int>(s.patterns.size());
  if (n == 0) return Result::Handled;
  const Value v = valueAt(m, 0);
  int cur = s.patternIndex(h_.currentPattern());
  if (kw(seg[1], "next") || kw(seg[1], "previous")) {
    if (!pressed(v)) return Result::Handled;
    cur = cur < 0 ? 0 : (cur + (kw(seg[1], "next") ? 1 : n - 1)) % n;
    h_.selectPattern(s.patterns[static_cast<size_t>(cur)].id);
    return Result::Handled;
  }
  if (kw(seg[1], "select")) {
    if (!m.args.empty() && m.args[0].text()) {
      const std::string want = normName(m.args[0].s);
      for (const auto& p : s.patterns)
        if (normName(p.name) == want) {
          h_.selectPattern(p.id);
          return Result::Handled;
        }
      if (!v.has) return Result::BadArgs;
    }
    if (!v.has) return Result::BadArgs;
    const int k = static_cast<int>(std::lround(v.v));
    if (k < 1 || k > n) return Result::BadArgs;
    h_.selectPattern(s.patterns[static_cast<size_t>(k - 1)].id);
    return Result::Handled;
  }
  return Result::Unknown;
}

OscControl::Result OscControl::marker(const std::vector<std::string>& seg, const osc::Message& m) {
  if (seg.size() != 2) return Result::Unknown;
  const Song& s = h_.doc().song();
  std::vector<const Marker*> ms;  // plain and action markers (meter and key markers are not jump targets)
  for (const auto& mk : s.markers)
    if (!mk.isMeter() && !mk.isKey()) ms.push_back(&mk);
  const Value v = valueAt(m, 0);
  auto jump = [&](const Marker* mk) {
    h_.setSongMode(true);
    h_.setPosition(static_cast<double>(mk->pos));
  };
  if (kw(seg[1], "next") || kw(seg[1], "previous")) {
    if (!pressed(v) || ms.empty()) return Result::Handled;
    const double pos = h_.position();
    const bool next = kw(seg[1], "next");
    const Marker* pick = nullptr;
    for (const Marker* mk : ms) {
      const double p = static_cast<double>(mk->pos);
      if (next && p > pos + 0.5 && (!pick || mk->pos < pick->pos)) pick = mk;
      if (!next && p < pos - 0.5 && (!pick || mk->pos > pick->pos)) pick = mk;
    }
    if (pick) jump(pick);
    return Result::Handled;
  }
  if (kw(seg[1], "jump")) {
    if (!m.args.empty() && m.args[0].text()) {
      const std::string want = normName(m.args[0].s);
      for (const Marker* mk : ms)
        if (normName(mk->name) == want) {
          jump(mk);
          return Result::Handled;
        }
      if (!v.has) return Result::BadArgs;
    }
    if (!v.has) return Result::BadArgs;
    const int k = static_cast<int>(std::lround(v.v));
    if (k < 1 || k > static_cast<int>(ms.size())) return Result::BadArgs;
    jump(ms[static_cast<size_t>(k - 1)]);
    return Result::Handled;
  }
  return Result::Unknown;
}

// ------------------------------------------------------------------ feedback ----

void OscControl::resetFeedback() {
  linkSent_.clear();
  for (auto& [a, w] : watch_) w.last = -1;
  sentPlaying_ = sentRecording_ = sentPattern_ = -1;
  sentTempo_ = -1;
  sentBars_ = -1e9;
  nextValues_ = nextPos_ = 0;
}

void OscControl::releaseNotes() {
  for (const auto& [ch, p] : held_) h_.note(ch, p, 0);
  held_.clear();
}

void OscControl::feedback(double now, std::vector<osc::Message>& out, double rate, double positionRate) {
  const size_t start = out.size();
  auto room = [&] { return out.size() - start < kMaxFeedbackPerCall; };
  if (pong_) {
    osc::Message p;
    p.address = "/gridline/pong";
    out.push_back(p);
    pong_ = false;
  }
  const Song& s = h_.doc().song();
  const int playing = h_.playing() ? 1 : 0, rec = h_.recording() ? 1 : 0;
  if (playing != sentPlaying_) out.push_back(msgF("/transport/play", playing)), sentPlaying_ = playing;
  if (rec != sentRecording_) out.push_back(msgF("/transport/record", rec)), sentRecording_ = rec;
  if (std::fabs(s.tempo - sentTempo_) > 1e-4) out.push_back(msgF("/transport/tempo", s.tempo)), sentTempo_ = s.tempo;
  const int pat = s.patternIndex(h_.currentPattern()) + 1;
  if (pat != sentPattern_) {
    osc::Message p;
    p.address = "/pattern/current";
    p.args.push_back(osc::Arg::Int(pat));
    out.push_back(p);
    sentPattern_ = pat;
  }
  if (now >= nextPos_) {
    nextPos_ = now + 1.0 / std::max(0.5, positionRate);
    const double t = h_.position();
    const MeterMap mm(s);
    const double bars = 1.0 + mm.barsAt(static_cast<Tick>(std::llround(t)));
    if (std::fabs(bars - sentBars_) > 1e-6) {
      sentBars_ = bars;
      out.push_back(msgF("/transport/position/bars", bars));
      out.push_back(msgF("/transport/position/beats", t / s.ppq));
      out.push_back(msgF("/transport/position/seconds", t / s.ppq * 60.0 / std::max(1.0, s.tempo)));
    }
  }
  if (now < nextValues_) return;
  nextValues_ = now + 1.0 / std::max(1.0, rate);
  for (const auto& l : s.links) {
    if (!room()) return;
    if (l.source != RemoteSource::Osc || l.action != RemoteAction::None || l.oscAddress.empty() || osc::isPattern(l.oscAddress) ||
        l.encoder != RemoteEncoder::Absolute)
      continue;
    const double n = paramNorm({l.owner, l.index});
    if (n < 0) continue;
    const double pos = linkPosition(l, n, l.curve == ModCurve::Formula ? formula(l.formula) : nullptr);
    const uint64_t key = RemoteMapper::key(l);
    auto it = linkSent_.find(key);
    if (it != linkSent_.end() && std::fabs(it->second - pos) < 1e-4) continue;
    linkSent_[key] = pos;
    out.push_back(msgF(l.oscAddress, pos));
  }
  for (auto& [address, w] : watch_) {
    if (!room()) return;
    double v = -1;
    if (w.kind == Watch::Solo) {
      const MixerTrack* t = s.mixerTrack(w.id);
      v = t ? (t->solo ? 1.0 : 0.0) : -1;
    } else {
      v = paramNorm(w.pid);
    }
    if (v < 0 || std::fabs(v - w.last) < 1e-5) continue;
    w.last = v;
    out.push_back(msgF(address, v));
  }
}

}  // namespace gl
