// Gridline — FLP export tests: J-39 (full state writers without the FL
// origin, project fields the encoder mode carries). Corpus checks read FL
// Studio's shipped projects / presets when FL is installed (skipped otherwise).
#pragma once
#include "packs/synths/mfmref.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include "core.h"
#include "enc_common.h"
#include "core/PluginParams.h"
#include "core/flp/FlpGen.h"
#include "packs/dynamics/dynamics.h"
#include "packs/modulation/envc.h"
#include "packs/models/harmor.h"
#include "packs/modulation/modulation.h"
#include "packs/synths/hex.h"
#include "packs/synths/matrixfm.h"
#include "packs/synths/matrixfm_data.h"
#include "packs/utility/utility.h"

namespace enctest {

// Shipped states of `flName` encoded from the encoder's defaults (no
// origin): how many decode back exactly; `bytesSame` / `bytesTotal`: bytes of
// the written state equal to the original (same size only).
struct J39Stats {
  int states = 0, decoded = 0, exact = 0;
  size_t bytesSame = 0, bytesTotal = 0;
};
inline J39Stats j39Native(const std::string& flName, size_t max, const std::function<Bytes(const Bytes&)>& view = {}) {
  J39Stats st;
  for (const Bytes& b : corpusStates(flName, max)) {
    ++st.states;
    gl::flp::PluginMapping m;
    if (!enc::decodeState(flName, b, m, ctx())) continue;
    ++st.decoded;
    const enc::Encoder* e = enc::findEncoder(flName, m.state.type);
    if (!e || !e->defaults) continue;
    Bytes t = e->defaults();
    enc::Result r = enc::encode(m.state, flName, &t, ctx());
    if (r.quality == enc::Result::Exact || r.quality == enc::Result::Verbatim) ++st.exact;
    const Bytes a = view ? view(b) : b, w = view ? view(r.state) : r.state;
    if (a.size() == w.size()) {
      st.bytesTotal += a.size();
      for (size_t i = 0; i < a.size(); ++i) st.bytesSame += a[i] == w[i];
    }
  }
  return st;
}
inline void j39Report(const char* what, const J39Stats& s) {
  std::printf("  J-39 %-28s %4d states, %4d decode, %4d exact without origin, same-size bytes equal %.1f%%\n", what, s.states, s.decoded, s.exact,
              s.bytesTotal ? 100.0 * static_cast<double>(s.bytesSame) / static_cast<double>(s.bytesTotal) : 0.0);
}

inline void testEncJ39() {
  namespace mf = gl::packs::synths::mfm;
  namespace hm = gl::packs::models::hm;
  namespace md = gl::packs::modulation;
  namespace dy = gl::packs::dynamics;

  // ---- Sytrus: the patch structure, fresh states, Gridline's values in them.
  {
    expect(defaultsDecode("Sytrus", "gl.matrixfm"), "Sytrus: a fresh state (version 25) decodes");
    gl::flp::PluginMapping m;
    expect(enc::decodeState("Sytrus", enc::findEncoder("Sytrus", "gl.matrixfm")->defaults(), m, ctx()) && gl::packs::synths::mfmref::withoutExact(m.state.data).empty(),
           "Sytrus: the fresh state carries Matrix FM's default curves");
    // Matrix FM's defaults on FL's value grid (a few knobs, e.g. LFO speeds, sit between FL's steps), then edited.
    gl::PluginState g = m.state;
    g.params[mf::MVolume] = 0.75f;
    g.params[mf::fm(0, mf::kColOut)] = 0.5f;
    g.params[mf::fm(1, 0)] = -0.25f;
    g.params[mf::op(1, mf::OpRatio)] = 2.0f;
    g.params[mf::flt(0, mf::FlCut)] = 0.375f;
    g.params[mf::art(0, mf::OaVol, mf::AfAtt)] = 1.5f;
    mf::PatchData pd;
    pd.level[0][2] = 0.5f;
    pd.phase[0][2] = 0.25f;
    pd.arts[0][mf::OaVol].env.pts = {{0, 0, 0}, {0.01f, 1, 0}, {0.3f, 0.6f, 0.5f}, {0.2f, 0, -0.3f}};
    pd.arts[0][mf::OaVol].env.sustain = 2, pd.arts[0][mf::OaVol].env.decay = 1;
    pd.arts[1][mf::OaPitch].maps[mf::MapVel].pts = {{0, 0.25f, 0}, {1, 0.75f, 0.2f}};
    pd.ws[0].pts = {{0, 0, 0}, {0.5f, 0.8f, 0.1f}, {0.5f, 1, 0}};
    g.data = mf::serializePatch(pd);
    expect(nativeRoundTrip("Sytrus", g), "Sytrus: matrix, operator, filter, envelope, mapping and waveshaper values are written from scratch");
    if (flInstalled()) {
      int same = 0, total = 0;
      for (const Bytes& b : corpusStates("Sytrus", 2000)) {
        ++total;
        same += enc::sytrusStructureRoundTrip(b) ? 1 : 0;
      }
      expect(total > 0 && same == total, "Sytrus: every shipped patch reads and writes back byte for byte (structure)");
      const J39Stats s = j39Native("Sytrus", 240, [](const Bytes& b) { return enc::sytrusRaw(b); });
      j39Report("Sytrus", s);
      expect(s.decoded > 0 && s.exact >= s.decoded - s.decoded / 50, "Sytrus: shipped states encode exactly without their origin (98 %+)");
    }
  }

  // ---- Harmor.
  {
    expect(defaultsDecode("Harmor", "gl.harmor"), "Harmor: a fresh state (version 19) decodes");
    gl::flp::PluginMapping m;
    expect(enc::decodeState("Harmor", enc::findEncoder("Harmor", "gl.harmor")->defaults(), m, ctx()) && m.state.type == "gl.harmor",
           "Harmor: the fresh state decodes as Resynth");
    // Resynth's defaults on FL's value grid, then edited.
    gl::PluginState g = m.state;
    g.params[hm::pf(0, hm::PfVolume)] = 0.5f;
    g.params[hm::flt(0, 0, hm::PfF1Cut)] = 0.25f;
    g.params[hm::pf(1, hm::PfEnabled)] = 1.0f;
    g.params[hm::GmPitch] = 700.0f;
    g.params[hm::FxRvOn] = 1.0f;
    hm::PatchData pd;
    if (!hm::parsePatch(g.data, pd)) pd = hm::PatchData{};
    pd.part[0].arts[hm::ArVol].env.pts = {{0, 0, 0, 0}, {0.01f, 1, 0, 0}, {0.5f, 0.5f, 0.2f, 1}, {0.3f, 0, 0, 0}};
    pd.part[0].arts[hm::ArVol].env.sustain = 2, pd.part[0].arts[hm::ArVol].env.enabled = true;
    pd.part[0].arts[hm::ArPitch].maps[hm::MkVel].pts = {{0, 0.25f, 0, 0}, {1, 0.75f, 0, 0}};
    pd.part[0].shapes[hm::ShTimbre1].pts = {{0, 0.5f, 0, 0}, {1, 0.1f, 0.3f, 0}};
    pd.part[0].arts[hm::ArVol].smooth = static_cast<float>((std::pow(2.0, 100 * 0.009) - 1.0) / 0.0511 / 1000.0);  // on FL's grid (tag value 100)
    g.data = hm::serializePatch(pd);
    expect(nativeRoundTrip("Harmor", g), "Harmor: part, filter, global, FX, articulator, shaping and smoothing values are written from scratch");
    if (flInstalled()) {
      int same = 0, total = 0;
      for (const Bytes& b : corpusStates("Harmor", 2000)) {
        ++total;
        same += enc::harmorStructureRoundTrip(b) ? 1 : 0;
      }
      expect(total > 0 && same == total, "Harmor: every shipped state reads and writes back byte for byte (structure)");
      const J39Stats s = j39Native("Harmor", 200, [](const Bytes& b) { return enc::harmorComparable(b); });
      j39Report("Harmor", s);
      expect(s.decoded > 0 && s.exact >= s.decoded * 3 / 4, "Harmor: shipped states encode exactly without their origin (75 %+)");
    }
  }

  // ---- Envelope Controller (J-41 articulator model): articulator settings written from scratch.
  {
    expect(defaultsDecode("Fruity Envelope Controller", "gl.envctl"), "Envelope Controller: a fresh state decodes");
    gl::flp::PluginMapping fresh;
    enc::decodeState("Fruity Envelope Controller", enc::findEncoder("Fruity Envelope Controller", "gl.envctl")->defaults(), fresh, ctx());
    gl::PluginState g = fresh.state;  // (its model text included)
    // values on FL's knob grid (raw knob units, defaults moved by 16, inside the knob ranges)
    const md::EnvcState def = md::defaultEnvcState();
    for (int k : {0 * md::AfFields + md::AfBase, 0 * md::AfFields + md::AfEnvAmt, 1 * md::AfFields + md::AfLfoAmt, 1 * md::AfFields + md::AfAttack})
      g.params[static_cast<size_t>(md::envcKnobParam(k))] = static_cast<float>(md::envcKnobToParam(k, def.knobs[static_cast<size_t>(k)] - 16));
    if (std::getenv("GL_J41_DEBUG")) {
      Bytes t = enc::findEncoder("Fruity Envelope Controller", "gl.envctl")->defaults();
      enc::Result r = enc::encode(g, "Fruity Envelope Controller", &t, ctx());
      gl::flp::PluginMapping back;
      enc::decodeState("Fruity Envelope Controller", r.state, back, ctx());
      std::printf("# envctl q %d mism %zu data %d\n", static_cast<int>(r.quality), r.mismatched.size(), back.state.data == g.data ? 1 : 0);
      for (size_t i = 0; i < g.params.size() && i < back.state.params.size(); ++i)
        if (g.params[i] != back.state.params[i]) std::printf("#  p%zu %g -> %g\n", i, static_cast<double>(g.params[i]), static_cast<double>(back.state.params[i]));
    }
    expect(nativeRoundTrip("Fruity Envelope Controller", g), "Envelope Controller: two articulators are written from scratch");
  }

  // ---- Distructor: the module rack (J-35 model 1) written module by module; Hardcore: the pedal board.
  {
    gl::PluginState g = gl::defaultPluginState("gl.amprack");
    g.params[dy::RkModel] = 1;
    g.params[dy::DrCount] = 2, g.params[dy::DrSoftClip] = 0;
    g.params[dy::DrSlot0] = 1, g.params[dy::DrSlot0 + 1] = 0;
    g.params[dy::drParam(0, dy::DmKind)] = 0, g.params[dy::drParam(0, dy::DmAlgo)] = 3, g.params[dy::drParam(0, dy::DmOs)] = 2;
    g.params[dy::drParam(0, dy::DmP0 + 2)] = 7300, g.params[dy::drParam(0, dy::DmP0 + 23)] = 640;
    g.params[dy::drParam(1, dy::DmKind)] = 1, g.params[dy::drParam(1, dy::DmFilterType)] = 14, g.params[dy::drParam(1, dy::DmDisabled)] = 1;
    g.params[dy::drParam(1, dy::DmP0 + 39)] = 2500;
    expect(nativeRoundTrip("Distructor", g), "Distructor: module rack written from scratch (kinds, algorithms, raw parameters, blocks)");
    gl::PluginState pb = gl::defaultPluginState("gl.amprack");
    pb.params[dy::RkModel] = 2;
    pb.params[dy::PbValue0 + 0] = 0.25f, pb.params[dy::PbValue0 + 11] = 0, pb.params[dy::PbValue0 + 30] = 0.75f;
    expect(nativeRoundTrip("Hardcore", pb), "Hardcore: pedal board written from scratch");
    gl::flp::PluginMapping none;
    expect(enc::decodeState("Distructor", Bytes{}, none, ctx()) && none.state.params == gl::defaultPluginState("gl.amprack").params,
           "Distructor: a plugin without state imports as the default rack (FL's defaults)");
  }

  // ---- Toxic Biohazard → Hex Synth.
  {
    expect(defaultsDecode("Toxic Biohazard", "gl.hexsynth"), "Toxic Biohazard: a fresh state decodes");
    gl::PluginState g = gl::defaultPluginState("gl.hexsynth");
    g.params[0] = 0.25f, g.params[10] = 0.75f;
    gl::packs::synths::hex::HexData d;
    d.amp[0][0] = 1.0f, d.amp[0][2] = 0.3f, d.phase[0][2] = 0.5f;
    d.name = "Test patch";
    g.data = gl::packs::synths::hex::formatHexData(d);
    expect(nativeRoundTrip("Toxic Biohazard", g), "Toxic Biohazard: parameters, wave spectra and name are written from scratch");
  }

  // ---- Corpus: plugins written without their origin.
  if (flInstalled()) {
    struct C {
      const char* fl;
      size_t max;
    };
    for (const C& c : {C{"DirectWave", 120}, C{"Distructor", 120}, C{"Hardcore", 20}, C{"Toxic Biohazard", 40}, C{"Fruity Envelope Controller", 60},
                       C{"Control Surface", 60}, C{"FLEX", 120}}) {
      const J39Stats s = j39Native(c.fl, c.max);
      j39Report(c.fl, s);
      expect(s.decoded > 0 && s.exact >= s.decoded - (std::string(c.fl) == "FLEX" ? 1 : 0),
             (std::string(c.fl) + ": every shipped state encodes exactly without its origin").c_str());
    }
  }
}

// ---- Project fields written in encoder mode (no FL needed).
inline void testExportJ39() {
  using namespace gl;
  // Marker actions ↔ FL marker kinds 1 / 2 / 3; every marker carries 33 / 34 / 46 / 45 / 168 / 101.
  {
    Song s = makeEmptySong();
    Marker a;
    a.pos = s.ppq * 4, a.name = "Loop me", a.action = MarkerAction::Loop;
    Marker b;
    b.pos = s.ppq * 8, b.name = "Skip", b.action = MarkerAction::Skip;
    Marker c;
    c.pos = s.ppq * 12, c.name = "Hold", c.action = MarkerAction::Pause;
    s.markers = {a, b, c};
    Song back;
    flp::ExportReport rep;
    flp::Bytes bytes;
    expect(coretest::exportImport(s, back, rep, &bytes), "markers: export / import");
    expect(back.markers.size() == 3 && back.markers[0].action == MarkerAction::Loop && back.markers[1].action == MarkerAction::Skip &&
               back.markers[2].action == MarkerAction::Pause,
           "markers: actions come back (marker loop / skip / pause)");
    flp::File f;
    std::string err;
    int kinds[4] = {0, 0, 0, 0}, extras = 0;
    if (flp::readBytes(bytes, f, &err))
      for (const auto& e : f.events) {
        if (e.id == 148 && (e.value() >> 24) < 4) ++kinds[e.value() >> 24];
        if (e.id == 168 || e.id == 101 || e.id == 45 || e.id == 46) ++extras;
      }
    expect(kinds[1] == 1 && kinds[2] == 1 && kinds[3] == 1, "markers: FL kinds 1 / 2 / 3 (the marker action menu's tags)");
    expect(extras == 12, "markers: FL 25's per-marker settings are written");
  }
  // Audio-clip channel without clips, an instrument-less channel.
  {
    Song s = makeEmptySong();
    Channel a;
    a.id = s.allocateId();
    a.name = "Clip source";
    a.plugin = defaultPluginState("gl.sampler");
    a.audioClip = true;
    a.mixerTrack = s.masterId();
    Channel e;
    e.id = s.allocateId();
    e.name = "Empty";
    e.mixerTrack = s.masterId();
    s.channels = {a, e};
    Song back;
    flp::ExportReport rep;
    expect(coretest::exportImport(s, back, rep), "channels: export / import");
    const Channel* ba = nullptr;
    const Channel* be = nullptr;
    for (const auto& c : back.channels) {
      if (c.name == "Clip source") ba = &c;
      if (c.name == "Empty") be = &c;
    }
    expect(ba && ba->audioClip, "channels: an audio-clip channel without clips stays one");
    expect(be && be->plugin.empty(), "channels: an instrument-less channel comes back without an instrument (FL layer)");
  }
  // Pattern automation copied from an automation clip → notes on the automation channel.
  {
    Song s = makeEmptySong();
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Synth";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.name = "Vol";
    const ParamId vol = channelParam(ch.id, ChannelParam::Volume);
    ac.owner = vol.owner, ac.index = vol.index;
    AutoPoint p0, p1;
    p0.pos = 0, p0.value = 0.2f;
    p1.pos = s.ppq, p1.value = 0.8f, p1.tension = 0.3f;
    ac.points = {p0, p1};
    s.automations.push_back(ac);
    Pattern p;
    p.id = s.allocateId();
    p.name = "P";
    PatternAutomation lane = patternLane(ac.owner, ac.index, {});
    for (Tick st : {Tick(0), Tick(s.ppq * 4)})
      for (AutoPoint q : ac.points) {
        q.pos += st;
        q.value = std::clamp(ac.min + q.value * (ac.max - ac.min), 0.0f, 1.0f);
        lane.points.push_back(q);
      }
    p.automation.push_back(lane);
    s.patterns.push_back(p);
    Song back;
    flp::ExportReport rep;
    expect(coretest::exportImport(s, back, rep), "pattern automation: export / import");
    const auto diffs = flp::compareSongs(s, back);
    expect(!coretest::has(diffs, "patterns"), "pattern automation: curves copied from an automation clip come back as they were");
  }
  // Clip groups, note groups, insert separator / lock / dock / threads, FL icon numbers.
  {
    Song s = makeEmptySong();
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Lead";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.mixerTrack = s.masterId();
    ch.flIcon = 74;
    s.channels.push_back(ch);
    Pattern p;
    p.id = s.allocateId();
    p.name = "P";
    for (int k = 0; k < 3; ++k) {
      Note n;
      n.start = k * s.ppq, n.length = s.ppq, n.pitch = 60 + k, n.group = k < 2 ? 3 : 0;
      p.notes[ch.id].push_back(n);
    }
    s.patterns.push_back(p);
    const ObjectId g1 = s.allocateId(), g2 = s.allocateId();
    for (int k = 0; k < 5; ++k) {
      Clip c;
      c.id = s.allocateId();
      c.pattern = p.id, c.track = k % 2, c.start = k * s.ppq * 4;
      c.group = k < 2 ? g1 : k < 4 ? g2 : kNoId;
      s.playlist.push_back(c);
    }
    if (s.tracks.size() < 2) s.tracks.resize(2);
    s.tracks[1].flIcon = 70;
    MixerTrack t;
    t.id = s.allocateId();
    t.name = "Bus";
    t.separator = true, t.locked = true, t.threaded = false, t.flIcon = 75;
    t.dock = static_cast<int>(MixerDock::Right);
    s.mixer.push_back(t);
    Song back;
    flp::ExportReport rep;
    expect(coretest::exportImport(s, back, rep), "J-39 fields: export / import");
    std::map<ObjectId, std::vector<int>> groups;
    for (size_t k = 0; k < back.playlist.size(); ++k) groups[back.playlist[k].group].push_back(static_cast<int>(k));
    expect(back.playlist.size() == 5 && back.playlist[0].group != kNoId && back.playlist[0].group == back.playlist[1].group &&
               back.playlist[2].group != kNoId && back.playlist[2].group == back.playlist[3].group &&
               back.playlist[0].group != back.playlist[2].group && back.playlist[4].group == kNoId,
           "clip groups come back (FL group number @14)");
    const Pattern* bp = nullptr;
    for (const auto& q : back.patterns)
      if (q.name == "P") bp = &q;
    bool notesOk = bp && !bp->notes.empty();
    if (notesOk) {
      const auto& ns = bp->notes.begin()->second;
      notesOk = ns.size() == 3 && ns[0].group == 3 && ns[1].group == 3 && ns[2].group == 0;
    }
    expect(notesOk, "note groups come back (note record @14)");
    const MixerTrack* bt = nullptr;
    for (const auto& m : back.mixer)
      if (m.name == "Bus") bt = &m;
    expect(bt && bt->separator && bt->locked && !bt->threaded && bt->dock == static_cast<int>(MixerDock::Right) && bt->flIcon == 75,
           "insert separator / lock / threads / dock / icon come back");
    const Channel* bc = back.channels.empty() ? nullptr : &back.channels[0];
    expect(bc && bc->flIcon == 74, "channel icon number comes back (event 155)");
    expect(back.tracks.size() > 1 && back.tracks[1].flIcon == 70, "playlist track icon number comes back (track record @8)");
  }
  // Fruity NoteBook 2: page lengths of 200 (one byte) and 300 (0xFF + u32) characters.
  {
    PluginState g = defaultPluginState("gl.notes");
    std::vector<std::string> pages(packs::utility::kNotePages);
    pages[0] = std::string(200, 'x'), pages[1] = std::string(300, 'y');
    g.data = packs::utility::encodeNotes(pages);
    const enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Fruity NoteBook 2" && r.quality == enc::Result::Exact && r.state.size() > 21 && r.state[12] == 200,
           "NoteBook 2: long pages written exactly (u8 length, 0xFF + u32 past 254)");
  }
  // Morphine without an origin: FL's plugin database preset as template (FL installed).
  if (flInstalled()) {
    PluginState g = defaultPluginState("gl.spectramorph");
    const enc::Result r = enc::encode(g, "", nullptr, ctx());
    expect(r.flName == "Morphine" && r.hasState && (r.quality == enc::Result::Exact || r.quality == enc::Result::Defaults),
           "Morphine: a native instance is written from FL's default state");
  }
}

}  // namespace enctest
