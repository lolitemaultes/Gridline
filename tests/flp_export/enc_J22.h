// Gridline — FLP export tests: J22 plugin encoders (FlpEncJ22.cpp).
#pragma once
#include <functional>
#include <sstream>
#include <utility>
#include "core/PluginInfo.h"
#include "core/flp/FlpGen.h"
#include "core/flp/FlpJ22Metal.h"
#include "enc_common.h"
#include "packs/models/drums.h"
#include "packs/models/metal.h"
#include "packs/models/strings.h"
#include "packs/synths/dco.h"
#include "packs/synths/va.h"

namespace enctest {

namespace j22t {

// The state the encoder's defaults decode to.
inline gl::PluginState decodedDefaults(const std::string& fl, const std::string& type) {
  const enc::Encoder* e = enc::findEncoder(fl, type);
  gl::flp::PluginMapping m;
  if (e && e->defaults) enc::decodeState(fl, e->defaults(), m, ctx());
  return m.state;
}
// Decoded defaults with some parameters (and the text form) changed round-trip natively.
inline bool moved(const std::string& fl, const std::string& type, const std::vector<std::pair<int, float>>& set,
                  const std::function<void(std::string&)>& data = {}) {
  gl::PluginState s = decodedDefaults(fl, type);
  if (s.params.empty()) return false;
  for (const auto& [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  if (data) data(s.data);
  return nativeRoundTrip(fl, s);
}
// A value at `pos` (0..1) of a continuous parameter, as the VA decoders read a stored position.
inline float at(const char* type, int k, float pos) {
  const gl::PluginInfo* p = gl::findPlugin(type);
  const auto& d = p->params[static_cast<size_t>(k)];
  return static_cast<float>(d.min + static_cast<double>(pos) * (d.max - d.min));
}
// First shipped state of `fl` with a changed text form re-encoded over the original (patch path).
inline bool patchData(const std::string& fl, const std::function<bool(gl::PluginState&)>& edit) {
  for (const Bytes& b : corpusStates(fl, 20)) {
    gl::flp::PluginMapping m;
    if (!enc::decodeState(fl, b, m, ctx())) continue;
    gl::PluginState g = m.state;
    if (!edit(g)) continue;
    enc::Result r = enc::encode(g, fl, &b, ctx());
    return r.quality == enc::Result::Exact;
  }
  return false;
}
inline void corpus(const std::string& fl, const char* name, int nativeShort = 0) {
  if (!flInstalled()) return;
  Stats s = corpusRoundTrip(fl, 400);
  expect(s.states > 0 && s.decoded == s.states, (std::string(name) + ": corpus decodes").c_str());
  expect(s.nativeExact == s.decoded - nativeShort, (std::string(name) + ": corpus native exact").c_str());
  expect(s.patchExact == s.decoded, (std::string(name) + ": corpus unchanged = verbatim").c_str());
}

}  // namespace j22t

inline void testEncJ22() {
  using namespace j22t;
  namespace dr = gl::packs::models::dr;
  namespace strs = gl::packs::models::strs;
  namespace va = gl::packs::synths::va;
  namespace dc = gl::packs::synths::dco;
  namespace mt = gl::packs::models::metal;

  // ---- drums
  expect(defaultsDecode("Fruit Kick", "gl.sweepkick"), "J22 Fruit Kick: defaults decode");
  expect(moved("Fruit Kick", "gl.sweepkick", {{dr::SkMaxFreq, 1200}, {dr::SkClick, 40}, {dr::SkSeed, 12345}}), "J22 Fruit Kick: native round trip");
  expect(defaultsDecode("BassDrum", "gl.kicksculpt"), "J22 BassDrum: defaults decode");
  expect(moved("BassDrum", "gl.kicksculpt", {{dr::KsLevel, 0.25f}, {dr::KsDrive, 0.75f}, {dr::KsNoiseSample, 3}}, [](std::string& d) { d = "Test kick"; }),
         "J22 BassDrum: native round trip (noise sample path, name)");
  expect(defaultsDecode("Drumpad", "gl.membranepad"), "J22 Drumpad: defaults decode");
  expect(moved("Drumpad", "gl.membranepad", {{dr::MdTension, 0.4f}, {dr::MdRetrig, 2}, {dr::MdVelSrc1, 3}, {dr::PdOutput, 0.6f}},
               [](std::string& d) { d = "Pad"; }),
         "J22 Drumpad: native round trip");
  expect(defaultsDecode("Drumaxx", "gl.membranekit"), "J22 Drumaxx: defaults decode");
  expect(moved("Drumaxx", "gl.membranekit", {{dr::kitPad(3, dr::KpKey), 50}, {dr::kitPad(3, dr::KpModel + dr::MdPitch), 0.3f}, {dr::KmSwing, 0.2f}},
               [](std::string& d) {
                 dr::KitData k;
                 dr::decodeKitData(d, k);
                 k.kitName = "My kit";
                 k.padName[2] = "Snare|Body";
                 k.pattern[1].length = 32;
                 k.pattern[1].key = -1;
                 k.pattern[1].cell[2][5] = 0x80 | 90;
                 d = dr::encodeKitData(k);
               }),
         "J22 Drumaxx: native round trip (kit text)");

  // ---- strings
  expect(defaultsDecode("Plucked!", "gl.plucker"), "J22 Plucked!: defaults decode");
  expect(moved("Plucked!", "gl.plucker", {{strs::PkDecay, 100}, {strs::PkColor, 20}, {strs::PkGate, 1}}), "J22 Plucked!: native round trip");
  expect(defaultsDecode("Sakura", "gl.twinstring"), "J22 Sakura: defaults decode");
  expect(moved("Sakura", "gl.twinstring", {{strs::TsLevel, 0.3f}, {strs::TsS2Ratio, 5}, {strs::TsTranspose, 1}, {strs::tsMod(0, strs::TmDest), 4}}),
         "J22 Sakura: native round trip");
  expect(defaultsDecode("Transistor Bass", "gl.stepbass"), "J22 Transistor Bass: defaults decode");
  expect(moved("Transistor Bass", "gl.stepbass", {{strs::SbCutoff, 0.3f}, {strs::SbRoot, 48}, {strs::SbSeqOn, 1}},
               [](std::string& d) {  // the current patch's line carries the same cutoff; a step of sequence 3 changes
                 std::istringstream in(d);
                 std::string line, out;
                 while (std::getline(in, line)) {
                   if (line.rfind("patch 0 ", 0) == 0) {
                     const size_t bar = line.find(" | ");
                     std::istringstream ls(line.substr(0, bar));
                     std::vector<std::string> t;
                     std::string w;
                     while (ls >> w) t.push_back(w);
                     t[2 + strs::SbCutoff] = "0.3";
                     std::string head;
                     for (size_t i = 0; i < t.size(); ++i) head += (i ? " " : "") + t[i];
                     line = head + line.substr(bar);
                   } else if (line.rfind("seq 3 ", 0) == 0) {
                     const size_t c = line.find(" 60:1:0:0");
                     line.replace(c, 9, " 67:1:1:1");
                   }
                   out += line + "\n";
                 }
                 d = out;
               }),
         "J22 Transistor Bass: native round trip (bank text)");

  // ---- VA
  expect(defaultsDecode("PoiZone", "gl.twinva"), "J22 PoiZone: defaults decode");
  expect(moved("PoiZone", "gl.twinva", {{va::TwCutoff, at("gl.twinva", va::TwCutoff, 0.25f)}, {va::TwFltMode, 1}, {va::TwBPitch, 7}, {va::TwTranspose, -5}},
               [](std::string& d) { d = "va 1\ncc 18 74\nname Test\n"; }),
         "J22 PoiZone: native round trip (CC links, name)");
  expect(defaultsDecode("Sawer", "gl.sawswarm"), "J22 Sawer: defaults decode");
  expect(moved("Sawer", "gl.sawswarm", {{va::SwCutoff, at("gl.sawswarm", va::SwCutoff, 0.5f)}, {va::SwTranspose, -5}, {va::SwModDest1, 10}},
               [](std::string& d) { d = "va 1\ncc 27 1\nname Swarm\n"; }),
         "J22 Sawer: native round trip (CC links, name)");
  expect(defaultsDecode("BooBass", "gl.fingerbass"), "J22 BooBass: defaults decode");
  expect(moved("BooBass", "gl.fingerbass", {{va::BbMid, static_cast<float>(1000 / 65535.0)}}), "J22 BooBass: native round trip");

  // ---- DCO
  expect(defaultsDecode("Fruity DX10", "gl.fmduo"), "J22 Fruity DX10: defaults decode");
  expect(moved("Fruity DX10", "gl.fmduo", {{dc::FdM1Coarse, static_cast<float>(30000 / 65536.0)}, {dc::FdOctave, -2}}), "J22 Fruity DX10: native round trip");
  expect(defaultsDecode("SimSynth", "gl.warmsynth"), "J22 SimSynth: defaults decode");
  expect(moved("SimSynth", "gl.warmsynth", {{dc::WmCut, 0.3f}, {dc::WmOsc2Coarse, -7}, {dc::WmOsc1Shape, 3}, {dc::WmRing, 1}, {dc::WmLfoShape, 1}}),
         "J22 SimSynth: native round trip");
  expect(defaultsDecode("Kepler", "gl.dcosynth"), "J22 Kepler: defaults decode");
  expect(moved("Kepler", "gl.dcosynth", {{dc::DcCutoff, 0.4f}, {dc::DcArpQuantize, 1}, {dc::DcChorusGate, 1}}), "J22 Kepler: native round trip");
  expect(defaultsDecode("Kepler Exo", "gl.dcosynth"), "J22 Kepler Exo: defaults decode");
  expect(moved("Kepler Exo", "gl.dcosynth", {{dc::DcCutoff, 0.4f}, {dc::DcMxSrc1, 3}, {dc::DcMxDst1, dc::kDcDestDco2Pitch}, {dc::DcMxSrc1 + 2, 5}, {dc::DcMxDst1 + 2, 38}}),
         "J22 Kepler Exo: native round trip (matrix rows)");
  {
    gl::PluginState one = decodedDefaults("Kepler", "gl.dcosynth"), two = decodedDefaults("Kepler Exo", "gl.dcosynth");
    const enc::Encoder* a = enc::nativeEncoder(one);
    const enc::Encoder* b = enc::nativeEncoder(two);
    expect(a && b && a->flName == "Kepler" && b->flName == "Kepler Exo", "J22 DCO Synth: panel picks the FL plugin");
  }

  // ---- metal
  expect(defaultsDecode("Fruity Granulizer", "gl.grainscan"), "J22 Fruity Granulizer: defaults decode");
  expect(moved("Fruity Granulizer", "gl.grainscan",
               {{mt::GsAttack, 300}, {mt::GsTrigger, 2}, {mt::GsTempoLock, 1}, {mt::GsEnvOn, 1},
                {mt::GsEnvAttack, static_cast<float>(gl::flp::gen::envSeconds(20000))}, {mt::GsEnvHold, static_cast<float>(gl::flp::gen::envSeconds(20000))},
                {mt::GsEnvDecay, static_cast<float>(gl::flp::gen::envSeconds(45000))}, {mt::GsEnvSustain, static_cast<float>(gl::flp::gen::envSustain(50))},
                {mt::GsEnvRelease, static_cast<float>(gl::flp::gen::envSeconds(20000))}}),
         "J22 Fruity Granulizer: native round trip (envelope)");
  expect(defaultsDecode("Wave Traveller", "gl.splinescratch"), "J22 Wave Traveller: defaults decode");
  expect(moved("Wave Traveller", "gl.splinescratch", {},
               [](std::string& d) {
                 mt::SsData s;
                 mt::parseScratchSpl(d, s);
                 mt::SsPatch& p = s.keys[60];
                 p.used = true;
                 p.beats = 8;
                 p.name = "Wiggle";
                 p.pos = {{0, 0, 0.5f, mt::SsBezier, true}, {0.5f, 0.75f, 0.25f, mt::SsLinear, false}, {1, 0.25f, 0.5f, mt::SsLinear, false}};
                 p.mutes = {{true, 0.0f}, {false, 0.5f}};
                 s.current = 60;
                 d = mt::serializeScratchSpl(s);
               }),
         "J22 Wave Traveller: native round trip (patch splines)");
  expect(defaultsDecode("Ogun", "gl.alloy") && defaultsDecode("Autogun", "gl.alloy"), "J22 Ogun / Autogun: defaults decode");
  for (const char* fl : {"Ogun", "Autogun"})
    expect(moved(fl, "gl.alloy", {{mt::AlCutoff, 100}, {mt::AlRichness, 5}, {mt::AlRvOn, 1}, {mt::AlComb, 3}, {mt::AlDlOn, 1}, {mt::alKnob(1, mt::AkAttack), 256}},
                 [](std::string& d) {
                   mt::AlloyData a;
                   mt::parseAlloy(d, a);
                   a.arts[1].att = 2;  // the knob above
                   a.arts[7].att = 2;
                   a.arts[7].maps[mt::AmVel].pts = {{0, 0.25f, 0, 0}, {1, 1, 0.5f, 0}};
                   a.arts[7].maps[mt::AmVel].enabled = true;
                   a.eq[mt::AeDecay].pts = {{0, 0.5f, 0, 0}, {0.5f, 0.8f, 0, 0}};
                   a.info = "Notes\nline two";
                   d = mt::serializeAlloy(a);
                 }),
           (std::string("J22 ") + fl + ": native round trip (params, articulators, notes)").c_str());
  {
    const enc::Encoder* e = enc::findEncoder("Morphine", "gl.spectramorph");
    expect(e && !e->defaults, "J22 Morphine: no default state (written without one natively)");
  }

  if (!flInstalled()) return;
  expect(patchData("Morphine", [](gl::PluginState& g) {
           mt::SmData d;
           if (!mt::parseMorph(g.data, d) || d.prog[0].zones.empty()) return false;
           d.prog[0].zones[0].gain = 0.5f;
           d.prog[2].zones = d.prog[0].zones;
           g.data = mt::serializeMorph(d);
           return true;
         }),
         "J22 Morphine: programs re-encoded over the original state");
  expect(patchData("Ogun", [](gl::PluginState& g) {
           g.params[mt::AlCutoff] = 50;
           g.params[mt::AlQuality] = 2;
           return true;
         }),
         "J22 Ogun: options patched inside the original record");
  corpus("Fruit Kick", "J22 Fruit Kick");
  corpus("Fruit kick", "J22 Fruit kick");
  corpus("BassDrum", "J22 BassDrum");
  corpus("Bassdrum", "J22 Bassdrum");
  corpus("Drumpad", "J22 Drumpad");
  corpus("DrumPad", "J22 DrumPad");
  corpus("Drumaxx", "J22 Drumaxx");
  corpus("Plucked!", "J22 Plucked!");
  corpus("Sakura", "J22 Sakura");
  corpus("Transistor Bass", "J22 Transistor Bass");
  corpus("PoiZone", "J22 PoiZone");
  corpus("Sawer", "J22 Sawer");
  corpus("BooBass", "J22 BooBass");
  corpus("Fruity DX10", "J22 Fruity DX10");
  corpus("SimSynth", "J22 SimSynth");
  corpus("Kepler", "J22 Kepler");
  corpus("Kepler Exo", "J22 Kepler Exo");
  corpus("Fruity Granulizer", "J22 Fruity Granulizer");
  corpus("Fruity granulizer", "J22 Fruity granulizer");
  corpus("Wave Traveller", "J22 Wave Traveller");
  corpus("Autogun", "J22 Autogun");
  {
    // Records older than version 10 leave out articulators (4..5 before
    // version 4, 11..12 before 10); the decoder gives those Gridline defaults
    // that a current record cannot express the same way (an LFO speed of
    // exactly 1 is not on the knob's grid): those stay approximate natively.
    int old = 0;
    for (const Bytes& b : corpusStates("Ogun", 400)) {
      gl::flp::j22::AlloyDecoded d;
      if (gl::flp::j22::decodeAlloyState(b.data(), b.size(), d, false) && d.version < 10) ++old;
    }
    corpus("Ogun", "J22 Ogun", old);
  }
  {
    Stats s = corpusRoundTrip("Morphine", 400);  // no default state: native = FL's defaults
    expect(s.states > 0 && s.decoded == s.states && s.patchExact == s.decoded, "J22 Morphine: corpus decodes, unchanged = verbatim");
  }
}

}  // namespace enctest
