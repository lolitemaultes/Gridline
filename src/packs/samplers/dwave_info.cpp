// Gridline — "samplers" pack, J-33 group "dwave": Wave Program catalogue
// entry and the program text codec (PluginState::data).
//   WP1
//   name <program name>
//   prog <play mode> <glide auto> <drive A on> <drive B on> <delay on> <ping-pong> <reverb on> <chorus on>
//        <clip volume> <compatible velocity> <sinc> <hq> <bend range> <cc7> <cc10> <cc11> <cc1>
//   plfo <k> <wave> <sync> <rate> <phase> <attack>
//   trig <group> <type> <probability> <overlap>      (groups that differ from the defaults)
//   Z <zone numbers…>   N <zone name>   F <sample path>   (one triple per zone)
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "../Packs.h"
#include "dwave.h"

namespace gl::packs::samplers {

namespace dw {

DwZone::DwZone() {
  slot[0] = {2, 2, 1.f};      // velocity → voice gain
  slot[1] = {3, 34, 0.75f};   // mod wheel → slot 3 amount
  slot[2] = {12, 1, 0.5f};    // zone LFO 2 → pitch
}

namespace {

// Every numeric zone field in one fixed order (shared by both directions).
template <class Z, class F>
void zoneFields(Z& z, F& f) {
  f(z.root), f(z.lo), f(z.hi), f(z.vlo), f(z.vhi), f(z.muteGroup), f(z.trigGroup), f(z.parallel), f(z.gain), f(z.pan);
  f(z.ticks), f(z.rate), f(z.loopMode), f(z.loopStart), f(z.loopEnd), f(z.sampleStart);
  f(z.tune), f(z.transpose), f(z.fine), f(z.keytrack), f(z.ts), f(z.tsSync), f(z.tsTime), f(z.tsGrain), f(z.tsSmooth);
  for (auto& v : z.send) f(v);
  for (auto& fl : z.f) f(fl.type), f(fl.cut), f(fl.res), f(fl.shape);
  f(z.beatSync), f(z.waveSlice);
  for (auto& v : z.amp) f(v);
  for (int k = 0; k < 4; ++k) f(z.fxOn[k]), f(z.fxA[k]), f(z.fxB[k]);
  for (auto& e : z.menv)
    for (auto& v : e) f(v);
  for (auto& l : z.lfo) f(l.wave), f(l.sync), f(l.rate), f(l.phase), f(l.attack);
  for (auto& s : z.slot) f(s.src), f(s.dst), f(s.amt);
}

struct Writer {
  std::string s;
  void operator()(int v) { s += " " + std::to_string(v); }
  void operator()(bool v) { s += v ? " 1" : " 0"; }
  void operator()(float v) {
    char b[32];
    std::snprintf(b, sizeof b, " %.9g", static_cast<double>(v));
    s += b;
  }
};
struct Reader {
  std::istringstream in;
  bool ok = true;
  double next() {
    double v = 0;
    if (!(in >> v)) ok = false;
    return v;
  }
  void operator()(int& v) { v = static_cast<int>(next()); }
  void operator()(bool& v) { v = next() != 0; }
  void operator()(float& v) { v = static_cast<float>(next()); }
};

}  // namespace

std::string formatProgram(const DwProgram& p) {
  std::string s = "WP1\nname " + p.name + "\n";
  char b[512];
  std::snprintf(b, sizeof b, "prog %d %d %d %d %d %d %d %d %d %d %d %d %d %.9g %.9g %.9g %.9g\n", p.playMode, p.glideAuto, p.driveOn[0],
                p.driveOn[1], p.delayOn, p.pingPong, p.revOn, p.choOn, p.clipVolume, p.compatVelocity, p.sinc, p.hq, p.bendRange,
                static_cast<double>(p.cc7), static_cast<double>(p.cc10), static_cast<double>(p.cc11), static_cast<double>(p.cc1));
  s += b;
  for (int k = 0; k < 2; ++k) {
    std::snprintf(b, sizeof b, "plfo %d %d %d %.9g %.9g %.9g\n", k, p.plfo[k].wave, p.plfo[k].sync, static_cast<double>(p.plfo[k].rate),
                  static_cast<double>(p.plfo[k].phase), static_cast<double>(p.plfo[k].attack));
    s += b;
  }
  for (int g = 0; g < 100; ++g) {
    const DwTrigger& t = p.trig[static_cast<size_t>(g)];
    if (t.type == 0 && t.prob == 1.f && t.overlap == 0.f) continue;
    std::snprintf(b, sizeof b, "trig %d %d %.9g %.9g\n", g, t.type, static_cast<double>(t.prob), static_cast<double>(t.overlap));
    s += b;
  }
  for (const DwZone& z0 : p.zones) {
    DwZone z = z0;
    Writer w;
    zoneFields(z, w);
    s += "Z" + w.s + "\nN " + z.name + "\nF " + z.path + "\n";
  }
  return s;
}

bool parseProgram(const std::string& text, DwProgram& p) {
  p = DwProgram{};
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line != "WP1") return false;
  while (std::getline(in, line)) {
    if (line.rfind("name ", 0) == 0) {
      p.name = line.substr(5);
    } else if (line.rfind("prog ", 0) == 0) {
      std::istringstream l(line.substr(5));
      int v[13] = {};
      for (int& x : v) l >> x;
      p.playMode = v[0], p.glideAuto = v[1], p.driveOn[0] = v[2], p.driveOn[1] = v[3], p.delayOn = v[4], p.pingPong = v[5];
      p.revOn = v[6], p.choOn = v[7], p.clipVolume = v[8], p.compatVelocity = v[9], p.sinc = v[10], p.hq = v[11], p.bendRange = v[12];
      l >> p.cc7 >> p.cc10 >> p.cc11 >> p.cc1;
    } else if (line.rfind("plfo ", 0) == 0) {
      std::istringstream l(line.substr(5));
      int k = 0, w = 0, sy = 0;
      l >> k >> w >> sy;
      if (k < 0 || k > 1) continue;
      p.plfo[k].wave = w, p.plfo[k].sync = sy != 0;
      l >> p.plfo[k].rate >> p.plfo[k].phase >> p.plfo[k].attack;
    } else if (line.rfind("trig ", 0) == 0) {
      std::istringstream l(line.substr(5));
      int g = -1;
      l >> g;
      if (g < 0 || g >= 100) continue;
      DwTrigger& t = p.trig[static_cast<size_t>(g)];
      l >> t.type >> t.prob >> t.overlap;
    } else if (line.rfind("Z ", 0) == 0 || line == "Z") {
      DwZone z;
      Reader r;
      r.in.str(line.size() > 1 ? line.substr(1) : std::string());
      zoneFields(z, r);
      if (!r.ok) return false;
      p.zones.push_back(z);
    } else if (line.rfind("N ", 0) == 0 && !p.zones.empty()) {
      p.zones.back().name = line.substr(2);
    } else if (line.rfind("F ", 0) == 0 && !p.zones.empty()) {
      p.zones.back().path = line.substr(2);
    }
  }
  return true;
}

}  // namespace dw

PluginInfo waveProgramInfo() {
  using namespace dw;
  auto pct = [](const char* n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Continuous, ParamScale::Linear, {}}; };
  std::vector<ParamDesc> q(DwCount, pct("(unused)", 0));
  q[DwMaster] = pct("Master volume", 1), q[DwGlide] = pct("Glide", 0);
  q[DwLfo1Rate] = pct("LFO 1 rate", 0.1), q[DwLfo1Phase] = pct("LFO 1 phase", 1), q[DwLfo1Attack] = pct("LFO 1 attack", 0);
  q[DwLfo2Rate] = pct("LFO 2 rate", 0.1), q[DwLfo2Phase] = pct("LFO 2 phase", 1), q[DwLfo2Attack] = pct("LFO 2 attack", 0);
  q[DwDelayTime] = pct("Delay time", 0.75), q[DwDelayFb] = pct("Delay feedback", 0.75), q[DwDelayLowCut] = pct("Delay low cut", 0.5);
  q[DwDelayHighCut] = pct("Delay high cut", 0.75);
  q[DwRevRoom] = pct("Reverb room", 0.25), q[DwRevDamp] = pct("Reverb damping", 0.5), q[DwRevDiffusion] = pct("Reverb diffusion", 0.75);
  q[DwRevDecay] = pct("Reverb decay", 0.25);
  q[DwChoDelay] = pct("Chorus delay", 0.25), q[DwChoDepth] = pct("Chorus depth", 0.5), q[DwChoRate] = pct("Chorus rate", 0.25);
  q[DwChoFb] = pct("Chorus feedback", 0);
  q[DwModVal1] = pct("Mod value 1", 0), q[DwModVal2] = pct("Mod value 2", 0), q[DwModVal3] = pct("Mod value 3", 0);
  q[DwModVal4] = pct("Mod value 4", 0);
  q[DwDriveA] = pct("Drive A", 0), q[DwDriveB] = pct("Drive B", 0);
  PluginInfo p{"gl.waveprogram", "Wave Program", "Sampler", PluginKind::Generator, std::move(q), false, {}};
  p.editorWidth = 700;
  return p;
}

}  // namespace gl::packs::samplers
