// Gridline - R08-01: one short automation lane must not make a bounce several times slower.
// The engine renders on a 64-frame control grid while links or automation could change a value;
// where every lane holds its value it renders long blocks, with the same output.
//   - AutomationLane::constantBetween: before the first point, after the last, past a clip's end,
//     before the first clip; false across a ramp, a point, an instance start or a looping clip.
//   - The same song renders the same with long blocks after the lane's last point as with control
//     blocks to the end (forced by a second, looping lane that keeps the pan where it is).
//   - Render time with one two-point lane stays within 1.5 times the time without it, or (on a
//     loaded computer) within half the extra time of control blocks everywhere.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include "Check.h"
#include "core/Automation.h"
#include "core/Params.h"
#include "core/Project.h"
#include "engine/Render.h"

using namespace gl;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

void addLane(Song& s, ChannelParam param, std::vector<AutoPoint> pts, Tick length, Tick loop) {
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = s.channels.front().id;
  a.index = static_cast<uint32_t>(param);
  a.points = std::move(pts);
  a.loop = loop;
  s.automations.push_back(a);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.start = 0, c.length = length, c.track = 0;
  c.loop = loop > 0;
  s.playlist.push_back(c);
}

double renderMs(const Song& s, const RenderOptions& o, AudioBuffer* out) {
  const auto t0 = std::chrono::steady_clock::now();
  AudioBuffer b = renderSong(s, o);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (out) *out = std::move(b);
  return ms;
}
}  // namespace

int main() {
  // ---- constantBetween
  {
    Song s = makeEmptySong();
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = s.masterId();
    a.index = static_cast<uint32_t>(MixerParam::Volume);
    a.points = {{96, 0.2f}, {384, 0.8f}};
    s.automations.push_back(a);
    Clip c;
    c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.start = 192, c.length = 768;
    s.playlist = {c};
    const auto lanes = automationLanes(s);
    expect(lanes.size() == 1, "one lane");
    const AutomationLane& L = lanes[0];
    expect(L.constantBetween(0, 191) && !L.constantBetween(0, 192), "before the first clip: its start value (to its start, not including it)");
    expect(L.constantBetween(192, 288), "before the first point");
    expect(!L.constantBetween(250, 300), "across the first point");
    expect(!L.constantBetween(300, 400), "along a ramp");
    expect(L.constantBetween(576, 900) && L.constantBetween(960, 5000), "after the last point and past the clip's end");
    expect(L.constantBetween(900, 576), "either order");
    s.automations[0].loop = 384;
    s.playlist[0].loop = true;
    expect(!automationLanes(s)[0].constantBetween(700, 900), "a looping clip is never treated as held");
    Clip c2 = c;
    c2.id = s.allocateId(), c2.start = 2000;
    s.automations[0].loop = 0;
    s.playlist = {c, c2};
    expect(!automationLanes(s)[0].constantBetween(1500, 2100), "a range that meets the next clip's start");
  }

  // ---- render: same output, no large slowdown
  Song base;
  std::string err;
  if (!loadProject("tests/fixtures/test_song.gridline.json", base, &err) || base.channels.empty()) {
    expect(false, "fixture song: " + err);
    return finish();
  }
  base.channels.front().pan = 0.0f;  // the pan lane below holds the centre (normalized 0.5)
  RenderOptions o;
  o.threads = 4;
  Song early = base;
  addLane(early, ChannelParam::Volume, {{0, 0.64f}, {699, 0.75f}}, 768, 0);
  Song held = early;
  addLane(held, ChannelParam::Pan, {{0, 0.5f}, {48, 0.5f}}, std::max<Tick>(base.lengthTicks(), 768), 96);
  expect(held.lengthTicks() == early.lengthTicks(), "the looping lane does not lengthen the song");
  // The three renders take turns, best of each: a loaded computer slows all three alike. (With
  // other programs running, every 64-frame block of the mixer threads waits longer for its turn;
  // the lane's own control-block stretch then costs more, but no more than its share of the
  // song rendered all in control blocks, which `held` measures under the same load.)
  AudioBuffer a, b, n;
  double tNone = 1e18, tLane = 1e18, tHeld = 1e18;
  for (int k = 0; k < 4; ++k) {
    tNone = std::min(tNone, renderMs(base, o, k == 0 ? &n : nullptr));
    tLane = std::min(tLane, renderMs(early, o, k == 0 ? &a : nullptr));
    tHeld = std::min(tHeld, renderMs(held, o, k == 0 ? &b : nullptr));
  }
  bool same = a.frames == b.frames && a.frames > 0;
  for (size_t i = 0; same && i < a.frames; ++i) same = a.at(0, i) == b.at(0, i) && a.at(1, i) == b.at(1, i);
  expect(same, "a lane that holds its value renders the same in long blocks as in control blocks");
  bool differs = false;
  for (size_t i = 0; !differs && i < std::min(a.frames, n.frames); ++i) differs = a.at(0, i) != n.at(0, i);
  expect(differs, "the lane is heard (the render differs from the song without it)");
  std::printf("render ms: none %.1f, one lane %.1f, all in control blocks %.1f\n", tNone, tLane, tHeld);
  // Unloaded, the lane costs at most half again the time without it. Loaded, the extra time is at
  // most half the extra of rendering the whole song in control blocks (the lane's points cover
  // 768 ticks of it): long blocks after the lane's last point are still in use.
  expect(tLane <= tNone * 1.5 + 5.0 || tLane - tNone <= 0.5 * (tHeld - tNone),
         "one two-point lane costs at most half again the time without it (or, on a loaded computer, half the cost of control blocks everywhere)");
  return finish();
}
