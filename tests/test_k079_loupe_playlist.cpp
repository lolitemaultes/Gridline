// Gridline tests — 0.7.9 Track 7 (K-418, K-419, K-420, K-421):
//   K-418: Playlist split/cut tool "snap to transients" (reusing Slicer's cached transient detector)
//   K-419: PlaylistWarp <-> Loupe editable markers round-trip & edits (add/move/delete/rename)
//   K-420: Loupe double-click marker selection (to next marker, or to end for last / single marker)
//   K-421: Loupe scalable layout (0.75x .. 2.0x, freely resizable) & persisted settings
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/Settings.h"
#include "core/Song.h"
#include "core/TimeWarp.h"
#include "engine/ClipOps.h"
#include "engine/PlaylistTools.h"
#include "engine/Sample.h"
#include "engine/Slicing.h"
#include "engine/WarpOps.h"
#include "packs/edison/edison.h"
#include "packs/edison/edison_dock.h"

using namespace gl;
namespace ed = gl::packs::edison;

namespace {

constexpr double kRate = 44100.0;

// Builds a 2-second percussive loop with sharp hits at 0.5 s, 1.0 s, 1.5 s.
ed::Audio makePercussiveAudio() {
  ed::Audio a = ed::makeAudio(kRate, 2, static_cast<int64_t>(2.0 * kRate));
  for (double at : {0.5, 1.0, 1.5}) {
    const int64_t start = static_cast<int64_t>(std::llround(at * kRate));
    for (int i = 0; i < 1600 && start + i < a.frames(); ++i) {
      const float env = static_cast<float>(std::exp(-i / 180.0));
      const float v = 0.85f * env * static_cast<float>(std::sin(i * 0.35));
      a.L[static_cast<size_t>(start + i)] = v;
      a.R[static_cast<size_t>(start + i)] = v * 0.95f;
    }
  }
  return a;
}

void testK418TransientSnap(const std::string& dir) {
  const ed::Audio au = makePercussiveAudio();
  const std::string wav = dir + "/k418_hits.wav";
  expect(ed::writeWavFile(wav, ed::makeContent(au), 32), "K-418: wrote percussive test wav");

  const SamplePtr smp = sampleCache().get(wav);
  expect(smp && smp->frames() == static_cast<size_t>(au.frames()), "K-418: loaded sample into SampleCache");
  if (!smp) return;

  // 1. Cached transient detector matches Slicer's computeSlices mode 0 and caches per SamplePtr.
  const std::vector<int64_t> direct = computeSlices(*smp, 0, 0.5);
  const auto cached1 = cachedSampleTransients(smp, 0.5);
  const auto cached2 = cachedSampleTransients(smp, 0.5);
  expect(cached1 && *cached1 == direct, "K-418: cachedSampleTransients matches Slicer computeSlices(mode 0)");
  expect(cached1.get() == cached2.get(), "K-418: repeated call reuses cached transient vector (0 recomputation)");
  expect(direct.size() >= 4, "K-418: Slicer detected all 3 interior hits plus start");

  // 2. Build a song with an audio clip and a pattern clip at 120 BPM (ppq 96 -> 1 beat = 96 ticks = 0.5 s).
  Song s;
  s.tempo = 120.0;
  s.ppq = 96;
  s.tracks.resize(4);
  Pattern pat;
  pat.id = s.allocateId();
  pat.name = "Pat 1";
  s.patterns.push_back(pat);
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Hits";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  s.channels.push_back(ch);

  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Audio;
  ac.source = ch.id;
  ac.track = 0;
  ac.start = 0;
  ac.length = 384;  // 2.0 s = 4 beats
  s.playlist.push_back(ac);

  Clip pc;
  pc.id = s.allocateId();
  pc.kind = ClipKind::Pattern;
  pc.pattern = s.patterns.front().id;
  pc.track = 1;
  pc.start = 0;
  pc.length = 384;
  s.playlist.push_back(pc);

  TempoMap tm;
  tm.build(s);

  // Pick the first interior detected transient (~0.5 s -> ~96 ticks).
  const int64_t hitFrame = direct.size() > 1 ? direct[1] : static_cast<int64_t>(0.5 * kRate);
  const Tick expectedTick = static_cast<Tick>(std::llround((static_cast<double>(hitFrame) / kRate) * 2.0 * s.ppq));

  // Pointer 4 ticks away from the transient (within 12 px at 0.5 px/tick -> 24 ticks reach).
  const pltools::TransientSnap nearSnap = pltools::snapCutToTransient(s, ac, tm, expectedTick + 4, 24.0, expectedTick + 4);
  expect(nearSnap.snapped && nearSnap.tick == expectedTick && nearSnap.sourceFrame == hitFrame,
         "K-418: cut snaps directly to nearest transient within pixel radius");

  // Pointer far from any transient (e.g. halfway between 0.5s and 1.0s at ~0.75s = 144 ticks, small reach 6 ticks).
  const Tick fallback = 144;
  const pltools::TransientSnap missSnap = pltools::snapCutToTransient(s, ac, tm, 144, 6.0, fallback);
  expect(!missSnap.snapped && missSnap.tick == fallback && missSnap.sourceFrame == -1,
         "K-418: cut outside transient snap radius falls back cleanly to grid/fallback tick");

  // Non-audio clip continues normal grid snap gracefully.
  const pltools::TransientSnap patSnap = pltools::snapCutToTransient(s, pc, tm, expectedTick + 2, 24.0, 96);
  expect(!patSnap.snapped && patSnap.tick == 96, "K-418: pattern clip ignores transient snap and keeps grid fallback");

  // Warped audio clip maps transient through warp markers cleanly.
  Clip wac = ac;
  wac.warp = {{1, 0.0, 0.0}, {2, 0.5, 0.75}, {3, 2.0, 2.0}};  // moves 0.5 s hit to 0.75 s (~144 ticks)
  const Tick warpedExpected = static_cast<Tick>(std::llround(clipTickOfSource(s, wac, tm, static_cast<double>(hitFrame) / kRate)));
  const pltools::TransientSnap warpSnap = pltools::snapCutToTransient(s, wac, tm, warpedExpected + 3, 24.0, warpedExpected + 3);
  expect(warpSnap.snapped && std::llabs(warpSnap.tick - warpedExpected) <= 1,
         "K-418: transient snap maps accurately through clip warp markers");

  // Modifier & toggle truth table.
  expect(!pltools::transientSnapActive(false, false, false), "K-418 mod: option off, no mod -> off");
  expect(!pltools::transientSnapActive(false, false, true), "K-418 mod: option off, Alt -> off (free)");
  expect(pltools::transientSnapActive(false, true, true), "K-418 mod: option off, Ctrl+Alt -> on");
  expect(pltools::transientSnapActive(true, false, false), "K-418 mod: option on, no mod -> on");
  expect(!pltools::transientSnapActive(true, false, true), "K-418 mod: option on, Alt -> off (bypass snap)");
  expect(!pltools::transientSnapActive(true, true, true), "K-418 mod: option on, Ctrl+Alt -> toggled off");

  // Cutting at the snapped tick splits the audio clip cleanly.
  const std::vector<ObjectId> rightIds = pltools::sliceClips(s, {ac.id}, nearSnap.tick, false);
  expect(rightIds.size() == 1, "K-418: sliceClips at transient tick produces right piece");
  const Clip* rightClip = rightIds.empty() ? nullptr : s.clip(rightIds.front());
  expect(rightClip && rightClip->start == nearSnap.tick, "K-418: right piece starts at exact snapped transient tick");

  Settings st;
  expect(!st.getBool("playlist.sliceSnapTransients"), "K-418: playlist.sliceSnapTransients defaults to false");
  expect(st.set("playlist.sliceSnapTransients", true) && st.getBool("playlist.sliceSnapTransients"),
         "K-418: playlist.sliceSnapTransients persists in Settings");
}

void testK419WarpLoupeRoundTrip() {
  ed::Content base = ed::makeContent(makePercussiveAudio());
  const std::vector<WarpMarker> orig = {{1, 0.0, 0.0}, {2, 0.5, 0.625}, {3, 1.5, 1.5}};

  // 1. Send to Loupe: import warp markers as editable Loupe markers.
  ed::Content imported = ed::importWarpMarkers(base, orig);
  expect(imported.markers.size() == 3, "K-419: imported 3 warp markers into Loupe");
  expect(imported.markers[0].pos == 0 && imported.markers[1].pos == static_cast<int64_t>(std::llround(0.5 * kRate)) &&
             imported.markers[2].pos == static_cast<int64_t>(std::llround(1.5 * kRate)),
         "K-419: Loupe marker sample positions match warp markers exactly");
  expect(imported.markers[0].name == "Warp 1" && imported.markers[1].name == "Warp 2" && imported.markers[2].name == "Warp 3",
         "K-419: imported markers have clean editable labels");

  // 2. Round-trip with zero edits preserves exact ids, source times, and warped times.
  const std::vector<WarpMarker> rt = ed::exportWarpMarkers(imported, orig);
  expect(rt == orig, "K-419: unmodified round-trip reproduces exact PlaylistWarp markers");

  // 3. Rename marker in Loupe, re-import same warp markers -> custom label preserved.
  ed::Content renamed = ed::renameMarker(imported, 1, "Snare Hit");
  ed::Content reimported = ed::importWarpMarkers(renamed, orig);
  expect(reimported.markers.size() == 3 && reimported.markers[1].name == "Snare Hit",
         "K-419: re-importing warp markers preserves custom Loupe marker names");

  // 4. Move marker in Loupe -> synced back with new sample position and preserved id.
  const int64_t movedPos = static_cast<int64_t>(std::llround(0.6 * kRate));
  ed::Content moved = ed::moveMarker(imported, 1, movedPos);
  const std::vector<WarpMarker> afterMove = ed::exportWarpMarkers(moved, orig);
  expect(afterMove.size() == 3 && afterMove[1].id == 2 && std::fabs(afterMove[1].at - 0.6) < 1e-4 && std::fabs(afterMove[1].to - 0.625) < 1e-4,
         "K-419: moving marker in Loupe updates warp marker source position cleanly");

  // 5. Add a marker in Loupe -> synced back with unique id and interpolated warp time.
  const int64_t addedPos = static_cast<int64_t>(std::llround(1.0 * kRate));
  ed::Content added = ed::addMarker(imported, addedPos, "Mid");
  const std::vector<WarpMarker> afterAdd = ed::exportWarpMarkers(added, orig);
  expect(afterAdd.size() == 4 && std::fabs(afterAdd[2].at - 1.0) < 1e-4 && afterAdd[2].to > afterAdd[1].to && afterAdd[2].to < afterAdd[3].to,
         "K-419: adding a marker in Loupe exports valid monotonic WarpMarker");

  // 5b. The added marker keeps its new id across syncs once the ids are written back (Loupe amends
  //     its markers from the exported list): a second sync exports the same list.
  {
    const ed::Content idsBack = ed::importWarpMarkers(added, afterAdd);
    expect(idsBack.markers.size() == 4 && idsBack.markers[2].name == "Mid", "ids written back keep the added marker and its name");
    expect(ed::exportWarpMarkers(idsBack, afterAdd) == afterAdd, "a second sync after the id write-back exports the same warp markers");
  }

  // 6. Delete a marker in Loupe -> removed from synced warp markers.
  ed::Content removed = ed::removeMarker(imported, 1);
  const std::vector<WarpMarker> afterRemove = ed::exportWarpMarkers(removed, orig);
  expect(afterRemove.size() == 2 && afterRemove[0].id == 1 && afterRemove[1].id == 3,
         "K-419: deleting a marker in Loupe removes it from synced warp markers");

  // 7. Clear all markers -> empty warp list.
  const std::vector<WarpMarker> afterClear = ed::exportWarpMarkers(ed::clearMarkers(imported), orig);
  expect(afterClear.empty(), "K-419: clearing all markers produces empty warp list");
}

void testK420DoubleClickMarkerSpan() {
  ed::Content c = ed::makeContent(ed::makeAudio(kRate, 2, 12000));
  c = ed::addMarker(c, 1000, "M1");
  c = ed::addMarker(c, 5000, "M2");
  c = ed::addMarker(c, 9000, "M3");

  const ed::Range r0 = ed::markerSpan(c, 0);
  const ed::Range r1 = ed::markerSpan(c, 1);
  const ed::Range r2 = ed::markerSpan(c, 2);
  expect(r0.a == 1000 && r0.b == 5000, "K-420: first marker selects to second marker");
  expect(r1.a == 5000 && r1.b == 9000, "K-420: middle marker selects to next marker");
  expect(r2.a == 9000 && r2.b == 12000, "K-420: last marker selects to end of audio");

  // Single marker selects from marker to end.
  ed::Content single = ed::makeContent(ed::makeAudio(kRate, 2, 12000));
  single = ed::addMarker(single, 4000, "Only");
  const ed::Range rs = ed::markerSpan(single, 0);
  expect(rs.a == 4000 && rs.b == 12000, "K-420: single marker selects from marker to end");

  // Unsorted markers still find the true next marker by sample position.
  ed::Content unsorted = ed::makeContent(ed::makeAudio(kRate, 2, 12000));
  unsorted.markers = {{9000, "C"}, {1000, "A"}, {5000, "B"}};
  const ed::Range ru = ed::markerSpan(unsorted, 1);
  expect(ru.a == 1000 && ru.b == 5000, "K-420: unsorted markers still select to next marker by sample position");

  // Boundary cases: out-of-range index and marker at end.
  const ed::Range oob = ed::markerSpan(c, 99);
  expect(oob.empty(), "K-420: out-of-range marker index returns empty range");
  ed::Content atEnd = ed::makeContent(ed::makeAudio(kRate, 2, 12000));
  atEnd = ed::addMarker(atEnd, 12000, "End");
  const ed::Range re = ed::markerSpan(atEnd, 0);
  expect(re.a == 12000 && re.b == 12000, "K-420: marker at exact end returns empty clamped range");
}

void testK421ScalableLoupe() {
  using namespace gl::packs::edison::dock;

  // Settings persistence.
  Settings st;
  expect(std::fabs(st.getNumber("loupe.uiScale") - 1.0) < 1e-6, "K-421: loupe.uiScale defaults to 1.0");
  expect(st.getNumber("loupe.windowW") == 0.0 && st.getNumber("loupe.windowH") == 0.0,
         "K-421: loupe.windowW/H default to 0 (auto)");
  expect(st.set("loupe.uiScale", 1.5) && std::fabs(st.getNumber("loupe.uiScale") - 1.5) < 1e-6,
         "K-421: loupe.uiScale stores and restores");
  expect(st.set("loupe.windowW", 1400.0) && st.set("loupe.windowH", 780.0), "K-421: loupe.windowW/H store and restore");

  // Layout across scale steps and window sizes.
  const Layout def = scaledLayout(1100, 560, 1.0f);
  expect(!def.compact && std::fabs(def.scale - 1.0f) < 1e-4f && def.formW == 1100.0f && def.headH == 112.0f && def.waveH == 440.0f,
         "K-421: default 1100x560 at 100% matches design geometry");

  const Layout grown = scaledLayout(1500, 820, 1.0f);
  expect(!grown.compact && std::fabs(grown.scale - 1.0f) < 1e-4f && grown.formW == 1500.0f && grown.waveH == 820.0f - grown.headH - grown.pad,
         "K-421: larger window expands waveform cleanly to fill width and height");

  const Layout s125 = scaledLayout(1400, 740, 1.25f);
  expect(!s125.compact && std::fabs(s125.scale - 1.25f) < 1e-4f && s125.headH > def.headH && s125.waveH > 500.0f,
         "K-421: 125% scale scales controls crisply with ample waveform height");

  const Layout s150 = scaledLayout(1700, 900, 1.5f);
  expect(!s150.compact && std::fabs(s150.scale - 1.5f) < 1e-4f && s150.headH > s125.headH,
         "K-421: 150% scale scales controls crisply");

  const Layout s200 = scaledLayout(2200, 1120, 2.0f);
  expect(!s200.compact && std::fabs(s200.scale - 2.0f) < 1e-4f && s200.headH == 224.0f,
         "K-421: 200% scale doubles design head height crisply");

  const Layout s75 = scaledLayout(825, 440, 0.75f);
  expect(!s75.compact && std::fabs(s75.scale - 0.75f) < 1e-4f && s75.headH < def.headH,
         "K-421: 75% scale reduces controls cleanly without compact wrapping");

  const Layout narrow = scaledLayout(480, 420, 1.0f);
  expect(narrow.compact && narrow.formW == 480.0f && narrow.headH + narrow.waveH + narrow.pad <= 420.5f,
         "K-421: narrow window switches cleanly to compact layout without clipping");

  bool fitsAll = true;
  for (float sc : kScaleSteps)
    for (float w : {480.0f, 800.0f, 1100.0f, 1440.0f, 1920.0f, 2400.0f})
      for (float h : {340.0f, 560.0f, 800.0f, 1200.0f}) {
        const Layout l = scaledLayout(w, h, sc);
        if (l.scale < 0.6f - 1e-4f || l.scale > sc + 1e-4f) fitsAll = false;
        if (l.formW > std::max(w, kMinW) + 0.5f) fitsAll = false;
        if (l.waveH < l.minWaveH - 0.5f) fitsAll = false;
      }
  expect(fitsAll, "K-421: scaledLayout invariants hold across all scales and window sizes");
}

}  // namespace

int main(int argc, char** argv) {
  // Helper mode for app tests: write a deterministic percussive WAV file.
  if (argc >= 3 && std::strcmp(argv[1], "writewav") == 0) {
    return ed::writeWavFile(argv[2], ed::makeContent(makePercussiveAudio()), 32) ? 0 : 1;
  }
  const std::string dir = gl::test::hermeticTestEnv("k079lp");
  testK418TransientSnap(dir);
  testK419WarpLoupeRoundTrip();
  testK420DoubleClickMarkerSpan();
  testK421ScalableLoupe();
  return finish();
}
