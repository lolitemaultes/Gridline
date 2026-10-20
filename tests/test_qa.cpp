// Gridline — J-21 QA: undo/redo stress over random Document edits, save/load
// round trips, parameter-id layout limits, and regression tests for bugs
// found by the end-to-end sweep and the reader fuzzers.
//
//   gridline_tests_qa [project.json...]   extra projects get the undo stress too
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/ChordTools.h"
#include "core/Fades.h"
#include "core/Grouping.h"
#include "core/MenuOpsExtra.h"
#include "core/MenuParity.h"
#include "core/MixerOps.h"
#include "core/NoteTools.h"
#include "core/SongTools.h"
#include "core/Timeline.h"
#include "core/WavWriter.h"
#include "core/ControllerScript.h"
#include "core/Demo.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/MenuOps.h"
#include "core/Params.h"
#include "core/PianoRollScript.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ClassicModFx.h"
#include "engine/Effects.h"
#include "engine/WavPack.h"
#include "packs/creative/creative.h"
#include "packs/dynamics/pocket_mods.h"

using namespace gl;
using namespace gl::ops;

namespace {

std::string text(const Song& s) { return projectToText(s); }

// ------------------------------------------------------------ random edits
struct Editor {
  Document& d;
  std::mt19937_64 rng;
  std::vector<std::string> gens, fxs, notefx;
  int applied = 0;
  bool newOps = true;  // J-36: mix in the operations added since J-21

  size_t pick(size_t n) { return n ? static_cast<size_t>(rng() % n) : 0; }
  int range(int lo, int hi) { return lo + static_cast<int>(pick(static_cast<size_t>(hi - lo + 1))); }
  const Song& s() const { return d.song(); }
  ObjectId anyChannel() { return s().channels.empty() ? kNoId : s().channels[pick(s().channels.size())].id; }
  ObjectId anyPattern() { return s().patterns.empty() ? kNoId : s().patterns[pick(s().patterns.size())].id; }
  ObjectId anyClip() { return s().playlist.empty() ? kNoId : s().playlist[pick(s().playlist.size())].id; }
  ObjectId anyMixer() { return s().mixer.empty() ? kNoId : s().mixer[pick(s().mixer.size())].id; }
  ObjectId anyInsert() { return s().mixer.size() < 2 ? kNoId : s().mixer[1 + pick(s().mixer.size() - 1)].id; }
  Tick anyTick() { return static_cast<Tick>(pick(static_cast<size_t>(s().ppq) * 64)); }

  ParamId anyParam() {
    switch (pick(6)) {
      case 0: return songParam(static_cast<SongParam>(pick(static_cast<size_t>(SongParam::Count))));
      case 1: return channelParam(anyChannel(), static_cast<ChannelParam>(pick(static_cast<size_t>(ChannelParam::Count))));
      case 2: {
        ObjectId c = anyChannel();
        for (const auto& ch : s().channels)
          if (ch.id == c && !ch.plugin.params.empty()) return channelPluginParam(c, static_cast<int>(pick(ch.plugin.params.size())));
        return channelParam(c, ChannelParam::Volume);
      }
      case 3: return mixerParam(anyMixer(), static_cast<MixerParam>(pick(static_cast<size_t>(MixerParam::Count))));
      case 4: {
        ObjectId t = anyMixer();
        for (const auto& m : s().mixer)
          if (m.id == t)
            for (int sl = 0; sl < static_cast<int>(m.slots.size()); ++sl)
              if (!m.slots[static_cast<size_t>(sl)].plugin.empty() && !m.slots[static_cast<size_t>(sl)].plugin.params.empty())
                return slotParam(t, sl, static_cast<int>(pick(m.slots[static_cast<size_t>(sl)].plugin.params.size())));
        return slotMixParam(t, static_cast<int>(pick(kEffectSlots)));
      }
      default: return sendParam(anyMixer(), static_cast<int>(pick(s().mixer.size())));
    }
  }

  std::set<ObjectId> someClips(int n = 3) {
    std::set<ObjectId> c;
    for (int k = 0; k < n; ++k) c.insert(anyClip());
    c.erase(kNoId);
    return c;
  }
  std::vector<ObjectId> someChannels(int n = 3) {
    std::set<ObjectId> c;
    for (int k = 0; k < n; ++k) c.insert(anyChannel());
    c.erase(kNoId);
    return {c.begin(), c.end()};
  }
  std::vector<ObjectId> someInserts(int n = 3) {
    std::set<ObjectId> c;
    for (int k = 0; k < n; ++k) c.insert(anyInsert());
    c.erase(kNoId);
    return {c.begin(), c.end()};
  }
  ObjectId anyClipOf(ClipKind k) {
    std::vector<ObjectId> v;
    for (const auto& c : s().playlist)
      if (c.kind == k) v.push_back(c.id);
    return v.empty() ? kNoId : v[pick(v.size())];
  }

  // J-36: the operations added since J-21 (J-27 menu ops, J-28 playlist
  // tools, J-31 groups / loops / key markers / chords, J-12 mixer ops).
  const char* stepNew() {
    switch (pick(37)) {
      case 30: {  // J-38/J-43: a fade edge edited by hand (length, shape, tension, declicker) or reset to automatic
        const ObjectId id = anyClipOf(ClipKind::Audio);
        const bool in = pick(2), reset = pick(4) == 0;
        const double len = pick(5) == 0 ? 0.0 : static_cast<double>(range(0, 4000)) / 1000.0;
        const float ten = static_cast<float>(range(-63500, 63500)) / 100.0f;
        const int shape = range(0, 1), decl = range(0, 2);
        d.editClip(id, reset ? "automatic crossfade" : "fade set by hand", [&](Clip& x) {
          double& l = in ? x.fadeIn : x.fadeOut;
          bool& m = in ? x.fadeInManual : x.fadeOutManual;
          if (reset) {
            l = 0, m = false;
            return;
          }
          l = len, m = true;
          (in ? x.fadeInShape : x.fadeOutShape) = static_cast<uint8_t>(shape);
          (in ? x.fadeInTension : x.fadeOutTension) = ten;
          (in ? x.fadeInDeclick : x.fadeOutDeclick) = static_cast<uint8_t>(decl);
        }, pick(2) ? 0 : 0x46414445ull + (in ? 1 : 0));  // half of them merge like a handle drag
        return "fadeEdit";
      }
      case 31: {  // J-38: an audio clip placed overlapping another one on its track (automatic crossfade)
        const ObjectId id = anyClipOf(ClipKind::Audio);
        const Clip* c = s().clip(id);
        if (!c) return "overlapClip-none";
        Clip k = *c;
        const Tick len = std::max<Tick>(1, s().clipLength(*c));
        k.start = c->start + range(1, static_cast<int>(std::min<Tick>(len, 1 << 20)));
        d.editSong("overlapping audio clip", [&](Song& x) {
          k.id = x.allocateId();
          k.fadeIn = k.fadeOut = 0, k.fadeInManual = k.fadeOutManual = false;
          x.playlist.push_back(k);
        });
        return "overlapClip";
      }
      case 32: d.editSong("automatic crossfades", [](Song& x) { x.autoCrossfades = !x.autoCrossfades; }); return "autoCrossfades";
      case 33: {  // J-39 icons: playlist track / channel FL icons, mixer track icon
        const int icon = range(-3, 300);
        switch (pick(3)) {
          case 0: d.editPlaylistTrack(range(-1, 12), "track icon", [&](PlaylistTrack& t) { t.flIcon = std::max(0, icon); }); break;
          case 1: d.editChannel(anyChannel(), "channel icon", [&](Channel& c) { c.flIcon = std::max(0, icon); }); break;
          default: mixops::setIcon(d, someInserts(), icon); break;
        }
        return "icons";
      }
      case 34: {  // J-39: insert lock / separator
        const bool sep = pick(2), on = pick(2);
        d.beginGroup(sep ? "separator" : "lock inserts");
        for (ObjectId t : someInserts(2))
          d.editMixerTrack(t, sep ? "separator" : "lock insert", [&](MixerTrack& m) { (sep ? m.separator : m.locked) = on; });
        d.endGroup();
        return "insertLockSeparator";
      }
      case 35: {  // J-31/J-39: clip groups moved as one (group drag), then possibly ungrouped
        d.beginGroup("group + drag");
        const ObjectId g = groupClips(d, someClips(4));
        const Tick dt = range(-s().ppq * 2, s().ppq * 2);
        if (g != kNoId)
          d.editClips("move group", [&](std::vector<Clip>& v) {
            for (auto& c : v)
              if (c.group == g) c.start = std::max<Tick>(0, c.start + dt);
          });
        d.endGroup();
        return "groupDrag";
      }
      case 36: {  // reset fades (clip menu) on a selection
        std::set<ObjectId> sel = someClips(4);
        d.editClips("reset fades", [&](std::vector<Clip>& v) {
          for (auto& x : v)
            if (sel.count(x.id)) x.fadeIn = x.fadeOut = 0, x.fadeInManual = x.fadeOutManual = false, x.fadeInShape = x.fadeOutShape = 0, x.fadeInTension = x.fadeOutTension = 0;
        });
        return "resetFades";
      }
      case 0: {  // automation clip (as the "Create automation clip" command makes it)
        ParamId pid = channelParam(anyChannel(), ChannelParam::Volume);
        Tick st = anyTick(), len = range(1, s().ppq * 16);
        int tr = range(0, 12);
        d.editSong("create automation clip", [&](Song& x) {
          AutomationClip c;
          c.id = x.allocateId();
          c.name = "auto";
          c.owner = pid.owner;
          c.index = pid.index;
          c.points = {AutoPoint{0, 0.2f, 0, AutoShape::Curve}, AutoPoint{len, 0.9f, 0, AutoShape::Curve}};
          Clip k;
          k.id = x.allocateId();
          k.kind = ClipKind::Automation;
          k.source = c.id;
          k.track = tr, k.start = st, k.length = len;
          while (static_cast<int>(x.tracks.size()) <= tr) x.tracks.push_back(PlaylistTrack{});
          x.automations.push_back(std::move(c));
          x.playlist.push_back(k);
        });
        return "addAutomationClip";
      }
      case 1: {  // audio clip on a channel (no sample file: the clip plays nothing)
        ObjectId ch = anyChannel();
        if (ch == kNoId) return "addAudioClip-none";
        Tick st = anyTick();
        int tr = range(0, 12);
        const Tick len = range(1, s().ppq * 8);
        d.editSong("add audio clip", [&](Song& x) {
          Clip k;
          k.id = x.allocateId();
          k.kind = ClipKind::Audio;
          k.source = ch;
          k.track = tr, k.start = st, k.length = len;
          k.audioEnd = 1.5;
          while (static_cast<int>(x.tracks.size()) <= tr) x.tracks.push_back(PlaylistTrack{});
          x.playlist.push_back(k);
        });
        return "addAudioClip";
      }
      case 2: {
        std::vector<ObjectId> pats;
        for (int k = 0; k < 3; ++k) pats.push_back(anyPattern());
        if (pick(3)) groupPatterns(d, pats, "g" + std::to_string(applied));
        else if (pick(2)) ungroupPatterns(d, pats);
        else deletePatternGroup(d, range(-1, 3));
        return "patternGroups";
      }
      case 3: if (pick(2)) groupClips(d, someClips()); else ungroupClips(d, someClips()); return "clipGroups";
      case 4: setStepLoop(d, anyPattern(), someChannels(), range(-2, 40)); return "setStepLoop";
      case 5: if (pick(2)) loopToContent(d, anyPattern(), someChannels(), range(0, 16)); else burnStepLoops(d, anyPattern(), someChannels()); return "loopToContent/burn";
      case 6: setKeyRange(d, anyChannel(), range(-5, 130), range(-5, 130)); return "setKeyRange";
      case 7: if (pick(2)) sliceClip(d, anyClip(), anyTick()); else sliceAt(d, anyTick(), range(-1, 12)); return "slice";
      case 8: slipClip(d, anyClip(), range(-s().ppq * 4, s().ppq * 4), pick(2) ? 0 : 77); return "slipClip";
      case 9: muteClips(d, someClips(), range(-1, 3)); return "muteClips";
      case 10: chopClips(d, someClips(2), range(-1, s().ppq * 2)); return "chopClips";
      case 11: splitClipByChannel(d, anyClip()); return "splitClipByChannel";
      case 12: if (pick(2)) insertSpace(d, anyTick(), range(-10, s().ppq * 8)); else deleteSpace(d, anyTick(), anyTick()); return "insert/deleteSpace";
      case 13: sortPatterns(d, pick(2) ? PatternSort::Name : PatternSort::Color); return "sortPatterns";
      case 14:
        switch (pick(4)) {
          case 0: addMarker(d, anyTick(), "m" + std::to_string(applied), range(0, 7), 1 << pick(4)); return "addMarker";
          case 1: removeMarker(d, range(-1, 5)); return "removeMarker";
          case 2: addKeyMarker(d, anyTick(), range(-2, 13), range(-1, 20)); return "addKeyMarker";
          default: addJumpToNextBar(d, anyTick()); return "addJumpToNextBar";
        }
      case 15: if (pick(2)) createLayer(d, someChannels()); else cutItself(d, someChannels()); return "createLayer/cutItself";
      case 16: {
        std::set<ObjectId> a;
        for (int k = 0; k < 2; ++k) a.insert(anyClipOf(ClipKind::Audio));
        a.erase(kNoId);
        switch (pick(4)) {
          case 0: pitchAudioClips(d, a, static_cast<float>(range(-2400, 2400))); return "pitchAudioClips";
          case 1: reverseAudioClips(d, a); return "reverseAudioClips";
          case 2: makeAudioClipUnique(d, anyClipOf(ClipKind::Audio)); return "makeAudioClipUnique";
          default: setAudioClipTimeMode(d, a, range(-1, 4)); return "setAudioClipTimeMode";
        }
      }
      case 17: {
        std::set<ObjectId> a;
        for (int k = 0; k < 3; ++k) a.insert(anyClipOf(ClipKind::Automation));
        a.erase(kNoId);
        if (pick(2)) mergeAutomationClips(d, a); else deleteUnusedAutomationClips(d);
        return "automationClipOps";
      }
      case 18:
        switch (pick(6)) {
          case 0: mixops::route(d, someInserts(2), someInserts(2), static_cast<mixops::RouteMode>(pick(4))); return "mixops::route";
          case 1: mixops::createSubmix(d, someInserts(), "sub"); return "mixops::createSubmix";
          case 2: mixops::insertTracks(d, range(-1, 10), range(0, 3)); return "mixops::insertTracks";
          case 3: mixops::moveSlotTo(d, anyMixer(), range(-1, kEffectSlots), anyMixer(), range(-1, kEffectSlots), pick(2)); return "mixops::moveSlotTo";
          case 4: mixops::setLatencyOffset(d, anyMixer(), range(-100, 5000)); return "mixops::setLatencyOffset";
          default: mixops::setGroup(d, someInserts(), pick(2) ? "grp" : ""); return "mixops::setGroup";
        }
      case 19:
        switch (pick(3)) {
          case 0: setTrackLocked(d, range(-1, 12), pick(2)); return "setTrackLocked";
          case 1: mergeTrackPatternClips(d, range(-1, 12)); return "mergeTrackPatternClips";
          default: muteTrackClips(d, range(-1, 12), pick(2)); return "muteTrackClips";
        }
      case 20: {  // J-28 playlist tools: one drag over a selection (move / resize / trim start / slip)
        std::set<ObjectId> sel = someClips();
        const Tick dt = range(-s().ppq * 4, s().ppq * 4);
        const int dtr = range(-2, 2), mode = range(0, 3);
        d.editClips("tool drag", [&](std::vector<Clip>& v) {
          for (auto& c : v) {
            if (!sel.count(c.id)) continue;
            switch (mode) {
              case 0: c.start = std::max<Tick>(0, c.start + dt), c.track = std::max(0, c.track + dtr); break;
              case 1: c.length = std::max<Tick>(1, (c.length ? c.length : s().ppq * 4) + dt); break;
              case 2: {
                Tick cut = std::clamp<Tick>(dt, 0, std::max<Tick>(0, (c.length ? c.length : s().ppq) - 1));
                c.start += cut, c.offset += cut;
                if (c.length) c.length -= cut;
                break;
              }
              default: c.offset = std::max<Tick>(0, c.offset + dt); break;
            }
          }
        }, pick(2) ? 0 : 0x7001);
        return "toolDrag";
      }
      case 21: if (pick(2)) convertStepsToNotes(d, anyPattern(), anyChannel()); else convertNotesToSteps(d, anyPattern(), anyChannel()); return "convertSteps/Notes";
      case 22: channelsToNewPattern(d, anyPattern(), someChannels(), pick(2)); return "channelsToNewPattern";
      case 23: {  // chord progression editor: writes the progression into a pattern (one merged step)
        chord::ChordProgression pr = chord::generate(range(0, 11), range(1, 6), range(1, 8), range(1, s().ppq * 8), 0.5, static_cast<uint32_t>(applied));
        chord::WriteOpts o;
        o.start = anyTick();
        o.mode = static_cast<chord::NonChordMode>(pick(3));
        ObjectId pt = anyPattern(), ch = anyChannel();
        std::vector<Note> base;
        if (const Pattern* pp = s().pattern(pt))
          if (auto it = pp->notes.find(ch); it != pp->notes.end()) base = it->second;
        d.setNotes(pt, ch, chord::write(base, pr, o), "chord progression", pick(2) ? 0x43485244ull : 0);
        return "chordProgression";
      }
      case 24: if (pick(2)) groupLayerChildren(d, anyChannel(), "lg"); else splitLayerChildren(d, anyChannel()); return "layerChildren";
      case 25: if (pick(2)) altSoloMixerTracks(d, someInserts()); else setSwingTruncate(d, someChannels(), range(-1, 3)); return "altSolo/swingTruncate";
      case 26: autoFillSteps(d, anyPattern(), anyChannel()); return "autoFillSteps";
      case 27: cloneChannelWithLinks(d, anyChannel()); return "cloneChannelWithLinks";
      case 28: addMarkersEvery(d, anyTick(), anyTick(), range(-1, s().ppq * 4)); return "addMarkersEvery";
      case 29: sendPatternClipsToTrack(d, anyPattern(), range(-1, 12)); return "sendPatternClipsToTrack";
      default: return "none";
    }
  }

  // One random edit; returns a short name (for failure messages).
  const char* step() {
    std::uniform_real_distribution<double> u(-0.2, 1.2);
    if (newOps && pick(3) == 0) return stepNew();
    switch (pick(46)) {
      case 0: d.addChannel("ch" + std::to_string(applied), gens[pick(gens.size())], range(-1, 5)); return "addChannel";
      case 1: if (s().channels.size() > 1) d.removeChannel(anyChannel()); return "removeChannel";
      case 2: d.moveChannel(anyChannel(), range(-2, static_cast<int>(s().channels.size()) + 2)); return "moveChannel";
      case 3: d.renameChannel(anyChannel(), "n" + std::to_string(applied)); return "renameChannel";
      case 4: d.setChannelPlugin(anyChannel(), gens[pick(gens.size())]); return "setChannelPlugin";
      case 5: d.cloneChannel(anyChannel()); return "cloneChannel";
      case 6: d.routeChannel(anyChannel(), anyMixer()); return "routeChannel";
      case 7: case 8: case 9: d.setParam(anyParam(), u(rng) * (pick(4) ? 1.0 : 400.0), pick(2) == 0); return "setParam";
      case 10: d.endGesture(); d.setParam(anyParam(), u(rng)); return "setParam-gesture";
      case 11: d.addPattern("p" + std::to_string(applied), range(1, 4), pick(2) ? 16 : 12, range(-1, 3)); return "addPattern";
      case 12: if (s().patterns.size() > 1) d.removePattern(anyPattern()); return "removePattern";
      case 13: d.clonePattern(anyPattern()); return "clonePattern";
      case 14: d.renamePattern(anyPattern(), "r" + std::to_string(applied)); return "renamePattern";
      case 15: d.setPatternLength(anyPattern(), range(0, 8), range(0, 32)); return "setPatternLength";
      case 16: d.setStep(anyPattern(), anyChannel(), range(-1, 70), static_cast<uint8_t>(pick(256))); return "setStep";
      case 17: case 18: {
        Note n;
        n.start = anyTick();
        n.length = range(0, s().ppq * 4);
        n.pitch = range(-5, 132);
        n.velocity = range(0, 127);
        d.addNote(anyPattern(), anyChannel(), n);
        return "addNote";
      }
      case 19: {
        std::vector<Note> v(pick(6));
        for (auto& n : v) n.start = anyTick(), n.pitch = range(0, 127);
        d.setNotes(anyPattern(), anyChannel(), v);
        return "setNotes";
      }
      case 20: case 21: d.addClip(anyPattern(), range(-1, 20), anyTick(), pick(2) ? 0 : anyTick()); return "addClip";
      case 22: d.removeClip(anyClip()); return "removeClip";
      case 23: d.moveClip(anyClip(), range(-1, 30), anyTick()); return "moveClip";
      case 24: d.setClipLength(anyClip(), range(-10, s().ppq * 16)); return "setClipLength";
      case 25: d.setPlaylistTrackCount(range(0, 200)); return "setPlaylistTrackCount";
      case 26: d.addMixerTrack(); return "addMixerTrack";
      case 27: if (s().mixer.size() > 3) d.removeMixerTrack(anyInsert()); return "removeMixerTrack";
      case 28: d.setSend(anyMixer(), anyMixer(), static_cast<float>(u(rng))); return "setSend";
      case 29: d.setSlotPlugin(anyMixer(), range(-1, kEffectSlots), pick(5) ? fxs[pick(fxs.size())] : std::string()); return "setSlotPlugin";
      case 30: d.moveSlot(anyMixer(), range(-1, kEffectSlots), range(-1, kEffectSlots)); return "moveSlot";
      case 31: d.setSendSidechain(anyMixer(), anyMixer(), pick(2)); return "setSendSidechain";
      case 32: if (!notefx.empty()) d.addNoteFx(anyChannel(), notefx[pick(notefx.size())], range(-1, 3)); return "addNoteFx";
      case 33: d.removeNoteFx(anyChannel(), range(-1, 3)); return "removeNoteFx";
      case 34: d.moveNoteFx(anyChannel(), range(-1, 3), range(-1, 3)); return "moveNoteFx";
      case 35: d.setPpq(pick(3) ? kPpqChoices[pick(kPpqChoices.size())] : range(1, 2000)); return "setPpq";
      case 36: d.setTimeSignature(range(0, 17), 1 << pick(6)); return "setTimeSignature";
      case 37: transposePattern(d, anyPattern(), range(-30, 30)); return "transposePattern";
      case 38: fillSteps(d, anyPattern(), anyChannel(), range(0, 8), range(-2, 5)); return "fillSteps";
      case 39: rotateChannel(d, anyPattern(), anyChannel(), pick(2) ? 1 : -1); return "rotateChannel";
      case 40: sortChannels(d, static_cast<ChannelSort>(pick(4))); return "sortChannels";
      case 41: insertPlaylistTrack(d, range(-1, 30)); return "insertPlaylistTrack";
      case 42: deletePlaylistTrack(d, range(-1, 30)); return "deletePlaylistTrack";
      case 43: {
        std::set<ObjectId> c;
        for (int k = 0; k < 3; ++k) c.insert(anyClip());
        c.erase(kNoId);
        switch (pick(3)) {
          case 0: duplicateClips(d, c); return "duplicateClips";
          case 1: mergeClips(d, c); return "mergeClips";
          default: removeClips(d, c); return "removeClips";
        }
      }
      case 44: makeClipUnique(d, anyClip()); return "makeClipUnique";
      default: {  // grouped burst
        d.beginGroup("burst");
        for (int k = 0; k < 3; ++k) {
          Note n;
          n.start = anyTick();
          d.addNote(anyPattern(), anyChannel(), n);
          d.setParam(anyParam(), u(rng));
        }
        d.endGroup();
        return "group";
      }
    }
  }
};

// J-46: an edited song exports to FLP (with / without its FL origin) and our
// own file reads back. Returns "" or what failed.
std::string flpExportReads(const Song& s) {
  for (bool origin : {true, false}) {
    if (!origin && s.flOrigin.empty()) continue;  // a Gridline-made song exports the same way in both modes
    flp::ExportOptions o;
    o.useOrigin = origin;
    o.factoryData = "/nonexistent-fl-data";
    flp::ExportReport rep;
    flp::Bytes out;
    if (!flp::exportFlp(s, out, rep, o)) return origin ? "export (origin) failed" : "export failed";
    Song back;
    flp::Report r2;
    std::string err;
    if (!importFlpBytes(out, back, r2, &err, "/nonexistent-fl-data", "", false)) return "re-import failed: " + err;
  }
  return "";
}

void undoStress(const Song& start, const char* tag, int edits, uint64_t seed) {
  const int checkEvery = std::getenv("GRIDLINE_QA_CHECK_EVERY") ? std::max(1, std::atoi(std::getenv("GRIDLINE_QA_CHECK_EVERY"))) : 25;
  Document d(start);
  d.setMaxUndoLevels(static_cast<size_t>(edits) + 10);
  Editor e{d, std::mt19937_64(seed), {}, {}, {}};
  for (const auto& p : pluginCatalog()) {
    if (p.kind == PluginKind::Generator) e.gens.push_back(p.type);
    else if (p.kind == PluginKind::Effect) e.fxs.push_back(p.type);
    else e.notefx.push_back(p.type);
  }
  const std::string original = text(d.song());
  // Hash of the state after each history position, to check every step back.
  std::vector<size_t> at{std::hash<std::string>()(original)};
  std::vector<std::string> what{"start"};
  bool valid = true;
  std::string firstInvalid, firstFlp;
  for (int i = 0; i < edits; ++i) {
    const char* op = e.step();
    ++e.applied;
    size_t pos = d.history().position();
    std::string t = text(d.song());
    at.resize(pos + 1);
    what.resize(pos + 1);
    at[pos] = std::hash<std::string>()(t);
    what[pos] = op;
    if (i == edits / 2 && firstFlp.empty())  // J-46: … and an exportable FL project
      if (std::string e = flpExportReads(d.song()); !e.empty()) firstFlp = std::string(op) + " (edit " + std::to_string(i) + "): " + e;
    if (i % checkEvery == 0) {  // the edited song must still be a loadable project
      Json j;
      Song back;
      std::string err;
      if (!jsonParse(t, j) || !songFromJson(j, back, &err) || text(back) != t) {
        if (valid) {
          firstInvalid = std::string(op) + " (edit " + std::to_string(i) + "): " + err;
          std::string b = text(back);
          size_t k = 0;
          while (k < t.size() && k < b.size() && t[k] == b[k]) ++k;
          size_t from = k > 160 ? k - 160 : 0;
          firstInvalid += "\n# saved:  ..." + t.substr(from, 240) + "\n# reload: ..." + b.substr(from, 240);
        }
        valid = false;
      }
    }
  }
  const std::string final = text(d.song());
  const size_t steps = d.history().position();
  std::string firstBad;
  for (size_t p = steps; p > 0; --p) {
    d.undo();
    if (firstBad.empty() && std::hash<std::string>()(text(d.song())) != at[p - 1])
      firstBad = "undo of '" + what[p] + "' (step " + std::to_string(p) + ")";
  }
  const bool backToStart = text(d.song()) == original;
  for (size_t p = 1; p <= steps; ++p) {
    d.redo();
    if (firstBad.empty() && std::hash<std::string>()(text(d.song())) != at[p])
      firstBad = "redo of '" + what[p] + "' (step " + std::to_string(p) + ")";
  }
  const bool backToFinal = text(d.song()) == final;
  std::string n = std::string(tag) + ": ";
  if (!firstBad.empty()) std::printf("# first mismatch: %s\n", firstBad.c_str());
  if (!valid) std::printf("# first invalid project: %s\n", firstInvalid.c_str());
  std::printf("# %s: %d edits, %zu history steps\n", tag, edits, steps);
  if (!firstFlp.empty()) std::printf("# first FLP export failure: %s\n", firstFlp.c_str());
  expect(valid, (n + "every edited state saves and loads back identically").c_str());
  expect(firstFlp.empty() && flpExportReads(d.song()).empty(), (n + "edited states export to FLP and read back").c_str());
  expect(firstBad.empty(), (n + "each undo / redo step restores the recorded state").c_str());
  expect(backToStart, (n + "undo all == original").c_str());
  expect(backToFinal, (n + "redo all == final").c_str());
}

// Parameter ids pack (owner, index); plugin params must not run into the next
// range (channel: note-processor base; slot: enable/mix indexes; note fx: stride).
void paramLayout() {
  size_t maxGen = 0, maxFx = 0, maxNote = 0;
  std::string gen, fx, note;
  for (const auto& p : pluginCatalog()) {
    size_t n = p.params.size();
    if (p.kind == PluginKind::Generator && n > maxGen) maxGen = n, gen = p.type;
    if (p.kind == PluginKind::Effect && n > maxFx) maxFx = n, fx = p.type;
    if (p.kind == PluginKind::NoteFx && n > maxNote) maxNote = n, note = p.type;
  }
  std::printf("# largest param counts: %s %zu, %s %zu, %s %zu\n", gen.c_str(), maxGen, fx.c_str(), maxFx, note.c_str(), maxNote);
  expect(kChannelPluginBase + maxGen <= kNoteFxBase, "generator params stay below the note-processor range");
  expect(maxFx <= kSlotEnable, "effect params stay below the slot enable / mix indexes");
  expect(maxNote <= kNoteFxStride, "note-processor params fit their stride");
}

void roundTrip(const Song& s, const char* tag) {
  std::string a = text(s);
  Json j;
  Song back;
  std::string err;
  bool ok = jsonParse(a, j) && songFromJson(j, back, &err);
  expect(ok && text(back) == a, (std::string(tag) + ": save -> load -> save is byte-identical").c_str());
}

// Bug: merging pattern clips far apart (or in a short time signature) made a
// pattern longer than a project file accepts; the saved project did not load.
void mergedPatternStaysLoadable() {
  Document d(makeDemoSong());
  ObjectId p = d.song().patterns[0].id;
  ObjectId a = d.addClip(p, 0, 0), b = d.addClip(p, 1, static_cast<Tick>(d.song().barTicks()) * 1500);
  ObjectId m = ops::mergeClips(d, {a, b});
  expect(m != kNoId, "merge clips 1500 bars apart");
  Json j;
  Song back;
  std::string err;
  expect(jsonParse(text(d.song()), j) && songFromJson(j, back, &err), "merged long pattern: project loads back");
  const Clip* c = d.song().clip(m);
  expect(c && c->length > static_cast<Tick>(d.song().barTicks()) * 1500, "merged clip keeps the whole span");
  expect(!d.setPatternLength(p, kMaxPatternBars + 1, 16) && !d.setPatternLength(p, 4, kMaxStepsPerBar + 1),
         "setPatternLength refuses lengths a project cannot store");
  expect(d.addPattern("x", kMaxPatternBars + 1) == kNoId, "addPattern refuses lengths a project cannot store");
}

// Bug (undo/save stress on converted FL projects): deleting a channel left its
// audio clips and layer references behind; the saved project silently lost
// them on load, so save -> load was no longer identical.
void removeChannelDropsReferences() {
  Document d(makeDemoSong());
  ObjectId ch = d.song().channels[0].id, layer = d.song().channels[1].id;

  d.editSong("fixture", [&](Song& s) {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch;
    c.start = 96;
    c.length = 96;
    s.playlist.push_back(c);
    s.channel(layer)->layer.push_back(ch);
  });
  const std::string withRefs = text(d.song());
  d.removeChannel(ch);
  Json j;
  Song back;
  std::string err;
  std::string t = text(d.song());
  bool same = jsonParse(t, j) && songFromJson(j, back, &err) && text(back) == t;
  bool gone = true;
  for (const auto& c : d.song().playlist) gone = gone && !(c.kind == ClipKind::Audio && c.source == ch);
  expect(same && gone && d.song().channel(layer)->layer.empty(), "removeChannel: audio clips and layer refs go with it (save/load identical)");
  d.undo();
  expect(text(d.song()) == withRefs, "removeChannel: undo restores clips and layer refs");

}

// Bugs from the reader fuzzers / sanitizer sweep: JSON numbers and casts.
void jsonNumbers() {
  // A subnormal value (e.g. a denormal parameter) was written fine but std::stod
  // threw ERANGE on reading it back, so the whole project failed to load.
  Json j;
  expect(jsonParse("[4.9406564584124654e-324, 1e-310, -2.5e-320]", j) && j.array.size() == 3,
         "JSON: subnormal numbers parse");
  expect(!jsonParse("[1e999]", j), "JSON: numbers that overflow a double are rejected");
  // Stringify cast |v| >= 2^63 to long long before the range check (UB) and
  // wrote NaN / inf as text no JSON reader accepts.
  Json arr = Json::makeArray();
  for (double v : {1e19, -1e19, 9.3e18, 1e300, std::nan(""), HUGE_VAL, 4.9406564584124654e-324, 42.0}) {
    Json n;
    n.type = Json::Type::Number;
    n.number = v;
    arr.array.push_back(n);
  }
  Json back;
  std::string t = jsonStringify(arr, 0);
  expect(jsonParse(t, back) && back.array.size() == 8, "JSON: every double serialises to parseable text");
  expect(back.array.size() == 8 && back.array[4].type == Json::Type::Null && back.array[0].number == 1e19 &&
             back.array[7].number == 42.0,
         "JSON: non-finite numbers become null, finite ones round-trip");
  expect(jsonInt<int>(1e300) == 2147483647 && jsonInt<int>(-1e300) == -2147483647 - 1 && jsonInt<uint64_t>(-1) == 0 &&
             jsonInt<int64_t>(9.3e18) == INT64_MAX && jsonInt<int>(std::nan("")) == 0 && jsonInt<int>(-7.9) == -7,
         "jsonInt saturates instead of overflowing");
  // Plugin parameters from a file: NaN / inf / huge choice indexes.
  ParamDesc c;
  c.kind = ParamKind::Choice;
  c.choices = {"a", "b", "c"};
  ParamDesc k;
  k.min = -1, k.max = 1, k.def = 0.25;
  expect(c.clamp(HUGE_VAL) == 2 && c.clamp(-HUGE_VAL) == 0 && c.clamp(std::nan("")) == 0 && c.clamp(1.4) == 1 &&
             c.clamp(1.6) == 2 && c.clamp(-0.4) == 0,
         "ParamDesc::clamp: choice indexes from any double");
  expect(k.clamp(std::nan("")) == 0.25 && k.clamp(HUGE_VAL) == 1 && k.clamp(-HUGE_VAL) == -1, "ParamDesc::clamp: NaN -> default");
  // Project with out-of-range numbers: loads (clamped / refused), no UB.
  Song s = makeDemoSong();
  Json pj = songToJson(s);
  for (auto& ch : pj.object["channels"].array) {
    ch.object["plugin"].object["params"].array.assign(3, Json());
    for (auto& v : ch.object["plugin"].object["params"].array) v.type = Json::Type::Number, v.number = 1e300;
    ch.object["mixer"].type = Json::Type::Number, ch.object["mixer"].number = 1e300;
  }
  Song out;
  std::string err;
  (void)songFromJson(pj, out, &err);
  expect(true, "project with huge numbers: no undefined behaviour (sanitizers)");
  // Browser index cache with hostile sizes / times / channel counts.
  std::string dir = gl::test::hermeticTestEnv("qa") + "/bi";
  std::string file = dir + "/index.json";
  std::filesystem::create_directories(dir);
  writeFileAtomic(file, R"({"format":"gridline-browser-index","version":99,"files":{"/x/a.wav":{"s":-1,"m":1e300,"ch":3e12,"key":-1e300}}})");
  browse::Index idx;
  idx.setCacheFile(file);
  browse::FileMeta m;
  expect(idx.load() && idx.lookup("/x/a.wav", m) && m.size == 0 && m.key == -1, "browser index: hostile cache values clamp");
}

// Every FL plugin decoder fed synthetic state payloads (no FL data): sizes
// around typical record boundaries, filled with extreme integers / floats.
// Under the sanitizers this pins the decoder overflow fixes from the fuzzers;
// in any build it checks every decoded state normalises to finite values.
void decoderSweep() {
  auto& all = flp::decoders();
  const size_t sizes[] = {0, 1, 3, 4, 7, 16, 33, 64, 128, 255, 400, 1024, 4099};
  const uint32_t words[] = {0u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x7F7FFFFFu /* FLT_MAX */, 0x7FC00000u /* NaN */,
                            0xFF800000u /* -inf */, 0x00000001u, 0x40000000u};
  std::mt19937 rng(5);
  size_t calls = 0, mapped = 0;
  bool finite = true;
  std::string bad;
  for (const auto& [name, dec] : all) {
    for (size_t sz : sizes)
      for (size_t w = 0; w <= sizeof words / sizeof words[0]; ++w) {
        std::vector<uint8_t> payload(sz);
        for (size_t i = 0; i < sz; ++i) {
          uint32_t v = w < sizeof words / sizeof words[0] ? words[w] : static_cast<uint32_t>(rng());
          payload[i] = static_cast<uint8_t>(v >> (8 * (i % 4)));
        }
        flp::File f;
        flp::Event st;
        st.id = 213;
        st.data = payload.data();
        st.size = static_cast<uint32_t>(sz);
        f.events.push_back(st);
        Song song;
        flp::Report rep;
        flp::Context c(f, song, rep);
        c.factoryData = "/nonexistent-fl-data";
        flp::PluginMapping m;
        m.flName = name;
        ++calls;
        if (!dec(c, f.events, m)) continue;
        ++mapped;
        if (m.state.type.empty()) m.state.type = m.glType;
        normalizePluginState(m.state);
        for (float v : m.state.params)
          if (!std::isfinite(v)) {
            if (finite) bad = name + " (" + std::to_string(sz) + " bytes)";
            finite = false;
          }
        if (m.param && m.value)
          for (int k : {0, 1, 7, 100, -1, 65535, 0x7FFFFFFF}) {
            int gi = m.param(k);
            (void)gi;
            (void)m.value(k, 0.5);
          }
        if (m.rawNorm) (void)m.rawNorm(1, 0x7FFFFFFF), (void)m.rawNorm(1, INT32_MIN);
      }
  }
  std::printf("# decoder sweep: %zu decoders, %zu calls, %zu mapped%s%s\n", all.size(), calls, mapped,
              bad.empty() ? "" : ", first non-finite: ", bad.c_str());
  expect(calls > 0, "every FL plugin decoder accepts arbitrary payloads (sanitizer-clean)");
  expect(finite, "decoded states normalise to finite parameters");
}

// Bug (fuzzer, sample reader): an Ogg Vorbis comment header with a huge comment
// count made stb_vorbis compute sizeof(char*) * count in int (wrapping to a
// tiny array it then wrote past) or allocate per comment for minutes.
void oggCommentCount() {
  auto page = [](uint8_t flags, uint32_t seq, const std::vector<uint8_t>& packet) {
    std::vector<uint8_t> p = {'O', 'g', 'g', 'S', 0, flags};
    for (int i = 0; i < 8; ++i) p.push_back(0);  // granule
    for (uint32_t v : {0x1234u, seq, 0u})        // serial, sequence, CRC (not checked when reading from memory)
      for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(v >> (8 * i)));
    p.push_back(1);
    p.push_back(static_cast<uint8_t>(packet.size()));
    p.insert(p.end(), packet.begin(), packet.end());
    return p;
  };
  auto le32 = [](std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  std::vector<uint8_t> id = {1, 'v', 'o', 'r', 'b', 'i', 's'};
  le32(id, 0);           // version
  id.push_back(1);       // channels
  le32(id, 44100);       // rate
  le32(id, 0), le32(id, 128000), le32(id, 0);
  id.push_back(0xB8);    // block sizes 2^8 / 2^11
  id.push_back(1);       // framing
  bool allFailFast = true;
  for (uint32_t count : {0x20000001u, 0x7FFFFFFFu, 0x10000001u, 0xFFFFFFFFu}) {
    std::vector<uint8_t> cm = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    le32(cm, 4);
    cm.insert(cm.end(), {'t', 'e', 's', 't'});
    le32(cm, count);
    for (int k = 0; k < 6; ++k) le32(cm, 3), cm.insert(cm.end(), {'a', '=', 'b'});
    cm.push_back(1);
    std::vector<uint8_t> file = page(2, 0, id);
    std::vector<uint8_t> p2 = page(0, 1, cm);
    file.insert(file.end(), p2.begin(), p2.end());
    DecodedAudio out;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = decodeOggVorbis(file.data(), file.size(), out, &err);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    allFailFast = allFailFast && !ok && s < 2.0;
  }
  expect(allFailFast, "Ogg Vorbis: corrupt comment count fails fast (no overflow, no allocation storm)");
}

// Bug (app sweep: "Backup failed: cannot replace …"): every writer of a file
// used the same "<file>.tmp", so concurrent writers (two instances sharing the
// backup folder; the browser index saved from its worker and the UI thread)
// wrote into one temp file and renamed it away from under each other.
void concurrentAtomicWrites() {
  std::string dir = gl::test::hermeticTestEnv("qa") + "/atomic";
  std::filesystem::create_directories(dir);
  const std::string file = dir + "/shared.json";
  std::atomic<int> failures{0};
  std::vector<std::thread> th;
  for (int k = 0; k < 6; ++k)
    th.emplace_back([&, k] {
      const std::string body(4096 + static_cast<size_t>(k) * 100, static_cast<char>('a' + k));
      for (int i = 0; i < 150; ++i)
        if (!writeFileAtomic(file, body)) ++failures;
    });
  for (auto& t : th) t.join();
  std::string got;
  {
    std::ifstream in(file, std::ios::binary);
    got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  bool whole = !got.empty() && got.size() == 4096 + static_cast<size_t>(got[0] - 'a') * 100 &&
               got.find_first_not_of(got[0]) == std::string::npos;
  size_t left = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) left += e.path().filename() != "shared.json";
  expect(failures == 0, "concurrent atomic writes of one file all succeed");
  expect(whole && left == 0, "the file holds one writer's complete text, no temp files left");
}

// J-36 regressions found by the extended undo stress / decoder sweep.
void j36Regressions() {
  {  // removePattern left the pattern's id in its pattern group: the saved
     // project named a missing pattern and loaded back different.
    Document d(makeDemoSong());
    std::vector<ObjectId> pats;
    for (const auto& p : d.song().patterns) pats.push_back(p.id);
    groupPatterns(d, {pats[0], pats[1]}, "g");
    const std::string before = text(d.song());
    d.removePattern(pats[0]);
    const auto& g = d.song().patternGroups;
    bool clean = true;
    for (const auto& x : g)
      for (ObjectId id : x.patterns) clean = clean && d.song().pattern(id);
    Json j;
    Song back;
    const std::string t = text(d.song());
    expect(clean && jsonParse(t, j) && songFromJson(j, back) && text(back) == t, "removePattern drops it from its pattern group");
    d.removePattern(pats[1]);
    expect(d.song().patternGroups.empty(), "removing a group's last pattern removes the empty group");
    d.undo();
    d.undo();
    expect(text(d.song()) == before, "undo restores the pattern group");
  }
  {  // removeChannel left its J-31 step loop entries in every pattern.
    Document d(makeDemoSong());
    const ObjectId pat = d.song().patterns[0].id, ch = d.song().channels[0].id;
    setStepLoop(d, pat, {ch}, 6);
    const std::string before = text(d.song());
    d.removeChannel(ch);
    Json j;
    Song back;
    const std::string t = text(d.song());
    expect(d.song().patterns[0].stepLoop.count(ch) == 0 && jsonParse(t, j) && songFromJson(j, back) && text(back) == t,
           "removeChannel drops its step loops");
    d.undo();
    expect(text(d.song()) == before, "undo restores the channel's step loop");
  }
  {  // Effector state with NaN floats: the clamp let NaN through to an int cast (fuzzer, fst target).
    std::vector<uint8_t> b{'S', 'S', 'L', 'F', 0, 0, 0, 0};
    b.resize(8 + 12 + 48, 0);
    const uint32_t len = static_cast<uint32_t>(b.size() - 8);
    for (int k = 0; k < 4; ++k) b[4 + static_cast<size_t>(k)] = static_cast<uint8_t>(len >> (8 * k));
    const uint8_t nanBytes[4] = {0x00, 0x00, 0xC0, 0x7F};
    for (size_t k = 0; k < 12; ++k) std::memcpy(&b[b.size() - 48 + 4 * k], nanBytes, 4);
    flp::Event ev;
    ev.id = 213;
    ev.data = b.data();
    ev.size = static_cast<uint32_t>(b.size());
    flp::File f;
    Song song;
    flp::Report rep;
    flp::Context c(f, song, rep);
    flp::PluginMapping m;
    bool ok = flp::decoders().at("Effector")(c, {ev}, m);
    bool finite = true;
    for (float v : m.state.params) finite = finite && std::isfinite(v);
    expect(ok && finite, "Effector decoder: NaN state floats decode to finite parameters");
  }
  {  // Harmor part with INT_MIN in the phaser / filter type fields: "v - 200" overflowed (UBSan; fuzzer, flp target).
    std::vector<uint8_t> b;
    auto put = [&](int32_t v) {
      for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
    };
    const int words = 386 + 12 + 13 + 4 + 8;
    put(16), put(1), put(words * 4);
    for (int i = 0; i < words; ++i) put(i == 386 + 12 + 10 || i == 386 + 12 + 13 + 2 || i == 386 + 12 + 13 + 3 ? INT32_MIN : 0);
    flp::Event ev;
    ev.id = 213;
    ev.data = b.data();
    ev.size = static_cast<uint32_t>(b.size());
    flp::File f;
    Song song;
    flp::Report rep;
    flp::Context c(f, song, rep);
    flp::PluginMapping m;
    (void)flp::decoders().at("Harmor")(c, {ev}, m);
    expect(true, "Harmor decoder: extreme phaser / filter type values (checked by UBSan)");
  }
  // FL phaser stage count: lround of a huge decoded value overflowed int.
  expect(classic::phaserStages(1e15) == 23 && classic::phaserStages(std::nan("")) == 3 && classic::phaserStages(-5) == 3 &&
             classic::phaserStages(7.0) == 8,
         "phaser stage count clamps huge / NaN raw values");
}

// J-36: the chord progression editor rewrote base + progression on every edit
// from the notes it captured when it opened, so notes drawn in the piano roll
// (or written by a script, or restored by undo) while it was open vanished at
// the next chord edit. chord::rebase carries those changes into the base.
void chordEditorKeepsOutsideEdits() {
  Document d(makeDemoSong());
  const ObjectId pat = d.song().patterns[0].id, ch = d.song().channels[0].id;
  auto notesNow = [&] {
    const Pattern* p = d.song().pattern(pat);
    auto it = p->notes.find(ch);
    return it == p->notes.end() ? std::vector<Note>{} : it->second;
  };
  std::vector<Note> base = notesNow();
  std::vector<Note> written = base;
  chord::ChordProgression prog = chord::generate(0, 1, 4, d.song().barTicks(), 0.0, 1);
  chord::WriteOpts o;
  o.mode = chord::NonChordMode::Keep;
  auto chordEdit = [&] {  // what ChordEditorUi::writeNow does
    base = chord::rebase(base, written, notesNow());
    std::vector<Note> w = chord::write(base, prog, o);
    d.setNotes(pat, ch, w, "chord progression", 0x43485244ull);
    written = w;
  };
  chordEdit();
  Note drawn;
  drawn.start = d.song().ppq * 3, drawn.length = 24, drawn.pitch = 101;  // drawn in the piano roll meanwhile
  d.addNote(pat, ch, drawn);
  chord::transpose(prog.chords[0], 2);
  chordEdit();
  const auto has = [&](const Note& n) { auto v = notesNow(); return std::find(v.begin(), v.end(), n) != v.end(); };
  expect(has(drawn), "chord editor: a note drawn while it is open survives the next chord edit");
  // A note deleted outside (not a chord note) stays deleted.
  std::vector<Note> v = notesNow();
  v.erase(std::find(v.begin(), v.end(), drawn));
  d.setNotes(pat, ch, v);
  chord::transpose(prog.chords[1], -1);
  chordEdit();
  expect(!has(drawn), "chord editor: a note deleted while it is open stays deleted");
  // Undo of the outside edit, then a chord edit: the undone state is the base.
  d.addNote(pat, ch, drawn);
  d.undo();
  chordEdit();
  expect(!has(drawn), "chord editor: an undone outside edit is not brought back");
  // Cancel restores the base including outside edits (not the notes from opening time).
  d.addNote(pat, ch, drawn);
  d.setNotes(pat, ch, chord::rebase(base, written, notesNow()), "cancel chord progression");
  expect(has(drawn), "chord editor: cancel keeps notes drawn while it was open");
}

// ------------------------------------------------------------- J-36: Lua sandbox
double secondsOf(const std::function<void()>& f) {
  auto t0 = std::chrono::steady_clock::now();
  f();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string luaErr(const std::string& code) {
  script::Limits lim;
  lim.seconds = 30.0;  // the checks below must stop on their own, not on the wall clock
  script::Vm vm(lim);
  std::string err;
  return vm.exec(code, "qa", &err) ? std::string() : err;
}

// Escapes from the instruction / time budget: work done in C where the
// count hook never fires, and finalizers (which Lua runs with hooks off).
void luaSandboxLimits() {
  std::string err;
  double t = secondsOf([&] { err = luaErr("return string.find(string.rep('a', 200000), '.-.-.-.-.-.-b')"); });
  std::printf("# catastrophic pattern stopped after %.2f s: %s\n", t, err.c_str());
  expect(err.find("pattern too complex") != std::string::npos && t < 20.0, "lua: backtracking pattern stops with an error");
  err = luaErr("for w in string.gmatch(string.rep('a', 100000), '(.-)(.-)(.-)b') do end");
  expect(err.find("pattern too complex") != std::string::npos, "lua: backtracking gmatch stops with an error");
  err = luaErr("return string.gsub(string.rep('ab', 50000), '(a-)(.-)(.-)(.-)c', 'x')");
  expect(err.find("pattern too complex") != std::string::npos, "lua: backtracking gsub stops with an error");
  expect(luaErr("assert(string.find(string.rep('a', 1000) .. 'b', '.-.-b') == 1)").empty(), "lua: ordinary patterns still match");
  err = luaErr("setmetatable({}, {__gc = function() while true do end end}) collectgarbage()");
  expect(err.find("__gc") != std::string::npos, "lua: __gc finalizers are refused");
  err = luaErr("local mt = {} mt.__gc = true setmetatable({}, mt)");
  expect(err.find("__gc") != std::string::npos, "lua: __gc refused when set before setmetatable");
  expect(luaErr("local mt = {} setmetatable({}, mt) mt.__gc = function() while true do end end collectgarbage()").empty(),
         "lua: __gc added after setmetatable never runs (Lua semantics)");
  t = secondsOf([&] { err = luaErr("table.move({}, 1, 1e12, 2)"); });
  expect(!err.empty() && t < 5.0, "lua: table.move over a huge range is refused");
  expect(luaErr("table.move({}, 1, '1e12', 2)").size() > 0, "lua: table.move with numeric strings is refused too");
  expect(luaErr("local a = table.move({1, 2, 3}, 1, 3, 2) assert(a[4] == 3 and a[2] == 1)").empty(), "lua: table.move still works");
}

// C++ glue that read Lua values outside a protected call: a metamethod that
// raised there called lua's panic handler and aborted the whole app.
void luaGlueProtected() {
  using namespace gl::script;
  PianoRollContext ctx;
  Note n0;
  n0.start = 0, n0.length = 96, n0.pitch = 60;
  ctx.notes = {n0};
  auto runScript = [&](const std::string& src, std::string* loadErr = nullptr) {
    PianoRollScript s;
    std::string e;
    if (!s.loadSource(src, "qa", &e)) {
      if (loadErr) *loadErr = e;
      PianoRollResult r;
      r.error = "load: " + e;
      return r;
    }
    Limits lim;
    lim.seconds = 30.0;
    return s.run(ctx, {}, lim);
  };
  PianoRollResult r = runScript("score._notes = { setmetatable({}, {__index = function() error('boom') end}) }");
  expect(!r.ok && r.error.find("boom") != std::string::npos, "piano roll script: note with a raising __index is an error, not abort");
  r = runScript("score._notes = { setmetatable({}, {__index = function() while true do end end}) }");
  expect(!r.ok && r.error.find("limit") != std::string::npos, "piano roll script: note with an endless __index hits the limit");
  r = runScript("score = setmetatable({}, {__index = function() error('nope') end})");
  expect(!r.ok, "piano roll script: score replaced by a raising object");
  r = runScript("score = 5");
  expect(!r.ok && !r.error.empty(), "piano roll script: score replaced by a number");
  r = runScript("setmetatable(_G, {__index = function(_, k) error('global ' .. k) end})");
  expect(r.ok, "piano roll script: _G with a raising __index (no apply / form lookups through it)");
  r = runScript("score._markers = { setmetatable({}, {__index = function() error('mk') end}) }");
  expect(!r.ok && r.error.find("mk") != std::string::npos, "piano roll script: marker with a raising __index");
  std::string le;
  runScript("function form(f) f._p = 'abcdefgh' end", &le);
  expect(!le.empty(), "piano roll script: form list replaced by a string is refused (was a type-confused rawgeti)");
  le.clear();
  runScript("function form(f) f._p = { 5, 6 } end", &le);
  expect(!le.empty(), "piano roll script: form entries that are not tables are refused");
  le.clear();
  runScript("function _gl_newForm() error('nf') end function form(f) end", &le);
  expect(le.find("nf") != std::string::npos, "piano roll script: raising _gl_newForm is an error");
  le.clear();
  runScript("function form(f) f._p = { setmetatable({}, {__index = function() error('ctl') end}) } end", &le);
  expect(le.find("ctl") != std::string::npos, "piano roll script: control with a raising __index is an error");
  r = runScript("function apply(s) s:add{pitch = 64} end");
  expect(r.ok && r.notes.size() == 2, "piano roll script: ordinary apply still works");

  Document doc(makeDemoSong());
  SimpleHost host(doc);
  ControllerScript cs;
  std::string err;
  bool loaded = cs.loadSource(
      "function onMidiMsg(m) setmetatable(m, {__index = function() error('handled?') end}) m.handled = nil end", "qa", host, &err);
  bool handled = true;
  cs.onMidiMsg(MidiMessage{0x90, 60, 100, 0}, &handled);
  expect(loaded && !handled && cs.error().find("handled?") != std::string::npos,
         "controller script: message table with a raising __index is reported, not abort");
}

}  // namespace


// ------------------------------------------------------------ J-46 (QA pass 3)
// Property: the per-clip fade resolver (one clip) and the whole-arrangement
// resolver (playback, drawing, FLP export) agree on every audio clip of
// random overlapping arrangements.
void j46FadeResolversAgree() {
  std::mt19937_64 rng(46);
  int bad = 0, xfades = 0;
  for (int round = 0; round < 300; ++round) {
    Song s = makeDemoSong();
    s.playlist.clear();
    const ObjectId ch = s.channels.empty() ? kNoId : s.channels[0].id;
    const int n = 2 + static_cast<int>(rng() % 9);
    for (int i = 0; i < n; ++i) {
      Clip c;
      c.id = s.allocateId();
      c.kind = ClipKind::Audio;
      c.source = ch;
      c.track = static_cast<int>(rng() % 2);
      c.start = static_cast<Tick>(rng() % 8) * 48;
      c.length = 48 * (1 + static_cast<Tick>(rng() % 6));
      c.audioEnd = 4;
      c.muted = rng() % 9 == 0;
      if (rng() % 4 == 0) c.fadeIn = 0.05, c.fadeInManual = true;
      s.playlist.push_back(c);
    }
    s.autoCrossfades = rng() % 5 != 0;
    TempoMap tm;
    tm.build(s);
    const std::vector<ClipFades> all = resolveAllFades(s, tm);
    for (size_t i = 0; i < s.playlist.size(); ++i) {
      const ClipFades one = resolveFades(s, tm, s.playlist[i]);
      auto same = [](const EdgeFade& a, const EdgeFade& b) {
        return a.partner == b.partner && a.crossfade == b.crossfade && a.automatic == b.automatic && std::fabs(a.len - b.len) < 1e-9 &&
               std::fabs(a.overlap - b.overlap) < 1e-9;
      };
      if (!same(one.in, all[i].in) || !same(one.out, all[i].out)) {
        if (++bad == 1) {
          std::printf("# fade resolver mismatch on clip %u (autoCrossfades %d):\n", static_cast<unsigned>(s.playlist[i].id), s.autoCrossfades);
          for (const Clip& c : s.playlist)
            std::printf("#   clip %u track %d start %lld len %lld muted %d fadeIn %.2f manual %d\n", static_cast<unsigned>(c.id), c.track,
                        static_cast<long long>(c.start), static_cast<long long>(c.length), c.muted, c.fadeIn, c.fadeInManual);
          std::printf("#   one: in %u %.3f out %u %.3f | all: in %u %.3f out %u %.3f\n", static_cast<unsigned>(one.in.partner), one.in.len,
                      static_cast<unsigned>(one.out.partner), one.out.len, static_cast<unsigned>(all[i].in.partner), all[i].in.len,
                      static_cast<unsigned>(all[i].out.partner), all[i].out.len);
        }
      }
      xfades += all[i].in.crossfade;
    }
  }
  std::printf("# fade resolvers: %d crossfades, %d disagreements\n", xfades, bad);
  expect(xfades > 100 && bad == 0, "fades: single-clip and whole-arrangement resolvers agree (partners, lengths)");
}

// Bug (fuzzer, fx-preset target; UBSan): the Love Philter reference engine
// converted -1.0 to uint32 for an LFO at full pulse width (the knob's maximum).
void j46OctaFilterFullPulse() {
  namespace cr = gl::packs::creative;
  bool finite = true;
  for (float pw : {1.0f, -1.0f, 0.999f}) {
    PluginState s = defaultPluginState("gl.octafilter");
    s.params[static_cast<size_t>(cr::OfModel)] = 1;
    for (int u = 0; u < cr::kUnits; ++u)
      for (int t = 0; t < cr::kTargets; ++t) s.params[static_cast<size_t>(cr::lfoParam(u, t, cr::LPulse))] = pw;
    auto fx = createEffect("gl.octafilter");
    ProcessContext ctx;
    ctx.sampleRate = 44100, ctx.playing = true;
    std::vector<float> L(512), R(512);
    for (int b = 0; b < 8; ++b) {
      for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i + 512 * static_cast<size_t>(b))));
      fx->process(s, ctx, L.data(), R.data(), 512);
      for (float x : L) finite = finite && std::isfinite(x);
    }
  }
  expect(finite, "love philter (reference engine): LFO at full pulse width runs (no UB) and stays finite");
}

// Bug (playlist input session, UBSan): a zoom-tool drag released outside the
// window converted floor(-FLT_MAX / zoom) = -inf to a tick.
void j46TickAtPointerOutside() {
  const float lost = -3.402823466e+38f;
  const Tick a = timeline::tickAtPx(lost, 0.5f), b = timeline::tickAtPx(-lost, 1e-6f), c = timeline::tickAtPx(std::nanf(""), 1.0f);
  expect(a < 0 && b > 0 && c == 0, "timeline: pixel -> tick is defined for an off-window pointer (-FLT_MAX), huge values and NaN");
  expect(timeline::tickAtPx(100.0f, 0.5f) == 200 && timeline::tickAtPx(-0.5f, 1.0f) == -1, "timeline: pixel -> tick floors like before");
}

// Bug (fuzzer, fst target on the shipped racks; UBSan): the Mobile Rack
// decoder's exactness check converted a huge stored module value to int.
void j46PocketExactHugeValue() {
  namespace pk = gl::packs::dynamics::pocket;
  float p[64] = {};
  p[0] = 1, p[1] = 9.3e35f;
  const bool a = pk::moduleExact(4, p), b = pk::moduleExact(40, p);
  p[1] = -9.3e35f;
  const bool c = pk::moduleExact(4, p), d = pk::moduleExact(40, p);
  p[1] = 0;
  expect(!a && !b && c && d && pk::moduleExact(4, p), "pocket rack: exactness check on huge / negative stored values (no UB, clamped)");
}

// Bug (fuzzer, song-flp target; UBSan): a project note at start = INT64_MAX
// overflowed start + length in the FLP exporter. Project ticks now load
// clamped to ±2^42.
void j46HugeTicksExport() {
  Song s = makeDemoSong();
  std::string t = projectToText(s);
  Json j;
  bool ok = jsonParse(t, j);
  int notes = 0, clips = 0;
  std::function<void(Json&, const std::string&)> walk = [&](Json& n, const std::string& key) {
    if (key == "notes")  // channel id → note list
      for (auto& [ch, list] : n.object)
        for (auto& x : list.array)
          if (auto it = x.object.find("start"); it != x.object.end()) it->second.number = 9.3e18, ++notes;
    if (key == "playlist")
      for (auto& x : n.array) {
        if (auto it = x.object.find("start"); it != x.object.end()) it->second.number = 1e300, ++clips;
        x.object["length"] = Json::makeNumber(9.2e18);
      }
    for (auto& [k, v] : n.object) walk(v, k);
    for (auto& v : n.array) walk(v, "");
  };
  walk(j, "");
  Song back;
  std::string err;
  ok = ok && notes > 0 && clips > 0 && songFromJson(j, back, &err);
  bool bounded = ok;
  for (const auto& c : back.playlist) bounded = bounded && c.start <= (Tick(1) << 42) && c.length <= (Tick(1) << 42);
  for (const auto& p : back.patterns)
    for (const auto& [ch, ns] : p.notes)
      for (const Note& n : ns) bounded = bounded && n.start <= (Tick(1) << 42);
  expect(bounded, "project load: tick values clamp to +-2^42 (no int64 overflow in start + length)");
  expect(ok && flpExportReads(back).empty(), "FLP export of a song with huge note / clip times: no overflow, reads back");
}

// Bug (fuzzer, fx-preset target; UBSan): the Love Philter reference shaper
// negated LONG_MIN when a runaway filter fed it a huge sample.
void j46OctaFilterRunaway() {
  namespace cr = gl::packs::creative;
  PluginState s = defaultPluginState("gl.octafilter");
  s.params[static_cast<size_t>(cr::OfModel)] = 1;
  auto fx = createEffect("gl.octafilter");
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  std::vector<float> L(256), R(256);
  for (float big : {1e30f, -1e30f, 3e38f, -3e38f}) {
    std::fill(L.begin(), L.end(), big), std::fill(R.begin(), R.end(), -big);
    fx->process(s, ctx, L.data(), R.data(), 256);
  }
  expect(true, "love philter (reference engine): huge input samples (no UB in the shaper)");
}

// Bug (J-47 report, first run with an empty config): the app quit while the
// browser index was still analysing (an empty cache: full pass over the
// shipped packs), leaving the worker to the index's static destructor while
// statics it uses were already gone. main now stops the index before
// teardown; stop() must end an analysing pass promptly.
void j46IndexStopsMidPass() {
  const std::string dir = gl::test::hermeticTestEnv("qa") + "/j46-index";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  AudioBuffer buf(2, 44100);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.01) * 0.3);
  for (int i = 0; i < 400; ++i) writeWav(dir + "/kick loop " + std::to_string(i) + ".wav", buf, 44100, 16);
  browse::Index ix;
  ix.setCacheFile(dir + "/index.json");
  ix.setRoots({{dir, true}});
  ix.start();
  for (int k = 0; k < 200 && ix.done() < 3; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const bool wasBusy = ix.busy();
  const auto t0 = std::chrono::steady_clock::now();
  ix.stop();
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("# index: stopped after %zu of 400 files in %.3f s\n", ix.done(), s);
  expect(wasBusy && !ix.busy() && ix.done() < 400 && s < 2.0, "browser index: stop() ends an analysing pass promptly (shutdown before teardown)");
  std::filesystem::remove_all(dir);
}

void j46Regressions() {
  j46FadeResolversAgree();
  j46IndexStopsMidPass();
  j46HugeTicksExport();
  j46OctaFilterRunaway();
  j46PocketExactHugeValue();
  j46TickAtPointerOutside();
  j46OctaFilterFullPulse();
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--types") {  // QA tooling: every built-in plugin type, one per line
    for (const auto& p : pluginCatalog()) std::printf("%s\n", p.type.c_str());
    return 0;
  }
  gl::test::hermeticTestEnv("qa");
  paramLayout();
  Song demo = makeDemoSong();
  roundTrip(demo, "demo song");
  mergedPatternStaysLoadable();
  removeChannelDropsReferences();
  jsonNumbers();
  decoderSweep();
  oggCommentCount();
  concurrentAtomicWrites();
  j36Regressions();
  luaSandboxLimits();
  chordEditorKeepsOutsideEdits();
  luaGlueProtected();
  j46Regressions();
  const int edits = std::getenv("GRIDLINE_QA_EDITS") ? std::atoi(std::getenv("GRIDLINE_QA_EDITS")) : 2000;
  undoStress(demo, "demo song", edits, 1);
  undoStress(Song{}, "empty song", edits / 4, 7);
  for (int i = 1; i < argc; ++i) {
    Song s;
    std::string err;
    if (!loadProject(argv[i], s, &err)) {
      std::printf("# cannot load %s: %s\n", argv[i], err.c_str());
      continue;
    }
    roundTrip(s, argv[i]);
    undoStress(s, argv[i], edits, 1000 + static_cast<uint64_t>(i));
  }
  return finish();
}
