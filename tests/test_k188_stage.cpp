// Gridline - the Stage in the mixer window: the stage's maths and grouping rules (core/StageMap),
// Arrange by sound (core/StageArrange), the Distance and Focus parameters, the pan laws, the shared
// room (pre-delay, early reflections, a level that keeps with distance), bit-exact old projects,
// save / load, undo, collaboration and the FL export bake.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/MixerOps.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/StageArrange.h"
#include "core/StageMap.h"
#include "core/StageRooms.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/DepthStage.h"
#include "engine/Effects.h"
#include "engine/Program.h"

using namespace gl;
namespace st = gl::stage;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

st::Geo geo(float scale = 1.0f, int tracks = 12) { return st::layout(4, 114, 1436, 688, scale, tracks); }

// ---------------------------------------------------------------- the stage on screen
void testGeometry() {
  const st::Geo g = geo();
  bool ok = true;
  for (double d = 0; d <= 1.0001; d += 0.0625) ok = ok && near(g.dOfY(g.yOf(d)), d, 1e-4);
  expect(ok, "distance -> y -> distance round trip");
  ok = true;
  for (double d : {0.0, 0.5, 1.0})
    for (double p = -1; p <= 1.0001; p += 0.125) ok = ok && near(g.panOfX(g.xOf(p, d), d), p, 1e-4);
  expect(ok, "pan -> x -> pan round trip at every distance");
  expect(g.yOf(0) > g.yOf(1) && g.yOf(0) < g.frontY && g.yOf(1) > g.backY, "the front row is lower than the back row, both on the stage");
  expect(g.yOf(0) - g.yOf(0.25) > g.yOf(0.75) - g.yOf(1.0), "rows sit a little closer at the back (mild perspective)");
  expect(near(g.side(0), 52, 0.5) && near(g.side(1), 36, 0.5), "tokens: 52 px at the front, 36 px at the back");
  expect(g.reach(0) > g.reach(1), "the stage is narrower at the back");
  expect(g.footRx(0, -1) >= g.side(0) * 0.5f + 10 - 0.01f && g.footRx(0, 0) > g.footRx(0, -0.5) && g.footRx(0, 1) > g.footRx(0, 0),
         "footprint: at least the token, wider with width");
  const st::Geo crowded = geo(1.0f, 32);
  expect(crowded.side(0) < g.side(0) && near(crowded.side(0), 52 * 0.86, 0.5), "25 or more tracks: tokens at 86 %");
  const st::Geo g2 = geo(2.0f);
  expect(g2.side(0) > g.side(0), "UI scale 2: larger tokens");
  expect(near(st::snapDepth(0.3), 0.25) && near(st::snapDepth(0.9), 1.0) && near(st::snapPan(0.02, 0.04), 0.0) && near(st::snapPan(0.3, 0.04), 0.3),
         "snap: five depths, a magnet at the centre");
}

// ---------------------------------------------------------------- groups
void testGroups() {
  std::vector<st::Pos> p = {{1, 0, 0}, {2, 0, 0}, {3, 0.5, 0.5}, {4, 0.01, 0.02}, {5, -0.5, 0.25}};
  auto gs = st::groups(p);
  expect(gs.size() == 1 && gs[0].size() == 3 && gs[0][0] == 0 && gs[0][1] == 1 && gs[0][2] == 3, "groups form from equal spots (within the tolerance)");
  // Every track at the default spot (a new or imported project): one group.
  std::vector<st::Pos> all;
  for (int k = 0; k < 12; ++k) all.push_back({static_cast<ObjectId>(10 + k), 0, 0});
  expect(st::groups(all).size() == 1 && st::groups(all)[0].size() == 12, "an imported project at the default spot: one group of 12");
  // Hysteresis: automation moving two members apart slowly.
  std::vector<st::Pos> two = {{1, 0, 0}, {2, 0, 0}};
  std::map<ObjectId, ObjectId> prev;
  int flips = 0;
  bool was = true;
  for (int step = 0; step <= 100; ++step) {  // 0 .. 0.1 apart and back, in 1/1000 steps with jitter
    const double off = (step <= 50 ? step : 100 - step) * 0.002 + ((step % 3) - 1) * 0.004;
    two[1].pan = std::max(0.0, off);
    auto g = st::groups(two, &prev);
    const bool on = !g.empty();
    if (on != was) ++flips;
    was = on;
    prev.clear();
    for (auto& m : g)
      for (size_t i : m) prev[two[i].id] = two[m.front()].id;
  }
  expect(flips == 2, "automation: a group dissolves once and forms once (hysteresis, no flicker)");
  std::vector<st::Pos> drift = {{1, 0, 0}, {2, 0.045, 0}};
  std::map<ObjectId, ObjectId> held = {{1, 1}, {2, 1}};
  expect(st::groups(drift).empty() && st::groups(drift, &held).size() == 1, "a member 0.045 away: no new group, but an earlier group holds");
  drift[1].pan = 0.07;
  expect(st::groups(drift, &held).empty(), "past the leave distance the group dissolves");
  const st::Pos s = st::spotOf(p, gs[0]);
  expect(near(s.pan, 0.01 / 3, 1e-9) && near(s.depth, 0.02 / 3, 1e-9), "a group's spot: the members' mean");
}

void testTray() {
  const st::Geo g = geo();
  const st::Pt spot = g.at(0, 0);
  const st::Tray t3 = st::trayLayout(g, spot, 3, 4, 1436, 114, 688);
  expect(t3.cols == 3 && t3.rows == 1 && t3.shown == 3 && t3.more == 0 && t3.above && t3.box.y1 < spot.y, "3 tracks: one row of 3, above the spot");
  const st::Tray t12 = st::trayLayout(g, spot, 12, 4, 1436, 114, 688);
  expect(t12.cols == 6 && t12.rows == 2 && t12.cells.size() == 12 && near(t12.box.w(), 6 * 96 + 22, 0.5), "12 tracks: 6 columns, 2 rows, 96 px cells");
  const st::Tray t16 = st::trayLayout(g, spot, 16, 4, 1436, 114, 688);
  expect(t16.cols == 8 && t16.rows == 2, "16 tracks: 8 columns");
  const st::Tray t40 = st::trayLayout(g, spot, 40, 4, 1436, 114, 688);
  expect(t40.cols == 10 && t40.rows == 3 && t40.shown == 29 && t40.more == 11 && t40.cells.size() == 30, "40 tracks: 3 rows of 10, the last cell +11");
  expect(st::trayCellAt(t12, t12.cells[7].x0 + 3, t12.cells[7].y0 + 3) == 7 && st::trayCellAt(t12, 0, 0) == -1, "cell hit test");
  const st::Tray back = st::trayLayout(g, g.at(0, 1), 12, 4, 1436, 114, 688);
  expect(!back.above && back.box.y0 > g.at(0, 1).y, "a group at the back: the tray hangs below its spot");
  bool inside = true;
  const st::Tray edge = st::trayLayout(g, g.at(1, 0), 12, 4, 1436, 114, 688);
  inside = edge.box.x1 <= 1436 + 0.5f && edge.box.x0 >= 4;
  expect(inside, "a tray near the edge stays inside");
}

// ---------------------------------------------------------------- drops
void testDrops() {
  const st::Geo g = geo();
  // Kick at centre near.
  std::vector<st::Obstacle> obs = {{g.token(0, 0.25), 0, 0.25, 7}};
  // A drop onto it: joins and takes its exact spot.
  const st::Pt over = g.at(0.02, 0.27);
  st::Drop d = st::resolveDrop(g, 0.02, 0.27, over, obs, false);
  expect(d.kind == st::Drop::Join && d.key == 7 && d.pan == 0 && d.depth == 0.25, "a drop onto a token joins it and copies its spot");
  // A near miss: a little overlap, the pointer beside the token: eased to a free spot, never overlapping.
  const double missPan = g.panOfX(g.xOf(0, 0.25) + g.side(0.25) * 0.85f, 0.25);
  const st::Pt beside{g.xOf(missPan, 0.25) + g.side(0.25) * 0.3f, g.yOf(0.25)};
  d = st::resolveDrop(g, missPan, 0.25, beside, obs, false);
  const float ov = st::overlapArea(g.token(d.pan, d.depth), obs[0].r);
  expect(d.kind == st::Drop::Nudged && ov == 0 && d.pan > 0 && std::fabs(d.depth - 0.25) < 1e-9, "a near miss: nudged to the nearest free spot, no overlap");
  // Alt (exact): any overlap joins.
  d = st::resolveDrop(g, missPan, 0.25, beside, obs, true);
  expect(d.kind == st::Drop::Join, "Alt drop with an overlap: joins");
  // Far away: free, exactly where dropped.
  d = st::resolveDrop(g, -0.7, 0.75, g.at(-0.7, 0.75), obs, false);
  expect(d.kind == st::Drop::Free && near(d.pan, -0.7) && near(d.depth, 0.75), "a drop on free floor stays where it was dropped");
  // A tray: the pointer over it joins the group.
  const st::Tray t = st::trayLayout(g, g.at(0, 0), 5, 4, 1436, 114, 688);
  std::vector<st::Obstacle> withTray = {{t.box, 0, 0, 100}};
  d = st::resolveDrop(g, 0.3, 0.1, {t.box.x0 + 10, t.box.y0 + 10}, withTray, false);
  expect(d.kind == st::Drop::Join && d.key == 100 && d.pan == 0 && d.depth == 0, "a drop over a tray joins the group");
  // Randomised: no drop ever leaves two tokens overlapping without joining.
  std::mt19937 rng(188);
  std::uniform_real_distribution<double> U(-1, 1), D(0, 1);
  std::vector<st::Obstacle> many;
  for (int k = 0; k < 10; ++k) {
    const double p = -0.9 + 0.2 * k, dd = (k % 3) * 0.5;
    many.push_back({g.token(p, dd), p, dd, k});
  }
  bool never = true;
  for (int k = 0; k < 400; ++k) {
    const double p = U(rng), dd = D(rng);
    const st::Drop r = st::resolveDrop(g, p, dd, g.at(p, dd + 0.2), many, false);
    if (r.kind == st::Drop::Join) continue;
    for (const auto& o : many) never = never && st::overlapArea(g.token(r.pan, r.depth), o.r) == 0;
  }
  expect(never, "400 random drops: joined or free, never resting on another token");
  // Display separation of near (not equal) spots.
  std::vector<st::Rect> rs = {g.token(0, 0.5), g.token(0.05, 0.5), g.token(0.08, 0.5)};
  const std::vector<float> off = st::separate(rs, 4);
  bool apart = true;
  for (size_t a = 0; a < rs.size(); ++a)
    for (size_t b = a + 1; b < rs.size(); ++b) {
      st::Rect A = rs[a], B = rs[b];
      A.x0 += off[a], A.x1 += off[a], B.x0 += off[b], B.x1 += off[b];
      apart = apart && st::overlapArea(A, B) == 0;
    }
  expect(apart, "tokens at close (not equal) spots are drawn apart");
}

// ---------------------------------------------------------------- labels
float mono7(const std::string& s) {  // 7 px per character (UTF-8 aware)
  float w = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) w += 7.0f;
  return w;
}

void testLabels() {
  const st::Measure m = mono7;
  expect(st::cutLabel("Kick", 100, m) == "Kick", "a short name is whole");
  const std::string a = st::cutLabel("Lead Vocal Double Left", 100, m, {"Lead Vocal Double Right"});
  const std::string b = st::cutLabel("Lead Vocal Double Right", 100, m, {"Lead Vocal Double Left"});
  std::printf("twins: %s | %s\n", a.c_str(), b.c_str());
  expect(a != b && a.find("Left") != std::string::npos && b.find("Right") != std::string::npos && mono7(a) <= 100 && mono7(b) <= 100,
         "twins keep the word that tells them apart");
  auto ellipses = [](const std::string& s) {
    int n = 0;
    for (size_t p = s.find("\xE2\x80\xA6"); p != std::string::npos; p = s.find("\xE2\x80\xA6", p + 1)) ++n;
    return n;
  };
  expect(ellipses(a) == 1 && ellipses(b) == 1, "one cut only, never two ellipses");
  const std::string n = st::cutLabel("Backing Vox Stack 1-8", 100, m);
  expect(n.size() > 3 && n.substr(n.size() - 3) == "1-8", "a numbered ending is kept");
  const std::string c = st::cutLabel("SuperSawLeadLayer03", 100, m);
  expect(c.substr(c.size() - 2) == "03", "digits ending a name without spaces are kept");
  const std::string p = st::cutLabel("Field Recording Rain Window", 100, m);
  expect(ellipses(p) == 1 && p.rfind("\xE2\x80\xA6") == p.size() - 3 && mono7(p) <= 100, "otherwise a plain end cut");
  const std::string cjk = st::cutLabel("\xE6\xBC\xA2\xE5\xAD\x97\xE6\xBC\xA2\xE5\xAD\x97\xE6\xBC\xA2\xE5\xAD\x97\xE6\xBC\xA2\xE5\xAD\x97\xE6\xBC\xA2\xE5\xAD\x97", 50, m);
  expect(mono7(cjk) <= 50 && (cjk.size() - 3) % 3 == 0, "CJK names are cut on whole characters");
  expect(st::initials("Super Saw") == "SS" && st::initials("kick") == "K" && st::initials("SuperSaw") == "SS", "initials for tiles without an icon");
  // 32 long names on a crowded stage at scale 1 and 2: labels never overlap.
  for (float scale : {1.0f, 2.0f}) {
    const st::Geo g = st::layout(4, 114, 4 + 1432 * scale, 114 + 574 * scale, scale, 32);
    const char* names[] = {"Kick In", "Kick Out", "Snare Top", "Snare Bottom", "Hats Closed", "Hats Open", "Clap Stack", "Tom Low",
                           "Overheads Left", "Overheads Right", "Room Mics", "Bass DI", "Bass Amp", "Electric Gtr Crunch L",
                           "Electric Gtr Crunch R", "Acoustic Guitar", "Felt Piano", "Rhodes", "Organ", "Warm Pad", "String Ensemble",
                           "Brass Section", "Lead Vocal Double Left", "Lead Vocal Double Right", "Backing Vox Stack 1-8", "Choir",
                           "Riser FX", "Impact FX", "Field Recording Rain", "SuperSawLeadLayer03", "Arp Synth", "Sub Drop"};
    std::vector<st::LabelIn> in;
    for (int k = 0; k < 32; ++k) {
      const double d = (k % 4) * 0.25, pan = -0.9 + 1.8 * ((k / 4) / 7.0);
      st::LabelIn l;
      l.name = names[k];
      l.token = g.side(d);
      l.cx = g.xOf(pan, d);
      l.y = g.yOf(d) + l.token * 0.5f + 7 * scale;
      l.minX = g.cx - g.halfAt(l.y);
      l.maxX = g.cx + g.halfAt(l.y);
      in.push_back(l);
    }
    const st::Measure ms = [scale](const std::string& s) { return mono7(s) * scale; };
    const auto out = st::layoutLabels(in, ms, scale);
    bool clean = true;
    for (size_t i = 0; i < out.size(); ++i)
      for (size_t j = i + 1; j < out.size(); ++j) {
        if (!out[i].visible || !out[j].visible) continue;
        const st::Rect A{out[i].x, out[i].y, out[i].x + out[i].w, out[i].y + 13 * scale}, B{out[j].x, out[j].y, out[j].x + out[j].w, out[j].y + 13 * scale};
        clean = clean && st::overlapArea(A, B) == 0;
      }
    int shown = 0;
    for (const auto& o : out) shown += o.visible;
    std::printf("scale %.0f: %d of 32 labels shown\n", static_cast<double>(scale), shown);
    expect(clean && shown >= 24, scale == 1.0f ? "32 labels at scale 1: none overlap" : "32 labels at scale 2: none overlap");
  }
}

// ---------------------------------------------------------------- arranging
void testSpread() {
  std::vector<st::Pos> t;
  for (int k = 0; k < 5; ++k) t.push_back({static_cast<ObjectId>(k + 1), 0.4, k < 3 ? 0.0 : 0.5});
  const auto s = st::spreadOut(t, 0.1);
  expect(near(s[0].pan, 0) && near(s[1].pan, -0.3) && near(s[2].pan, 0.3), "spread out: centre, then left and right in turn, in mixer order");
  expect(near(s[3].pan, -0.15) && near(s[4].pan, 0.15) && s[3].depth == 0.5 && s[0].depth == 0.0, "spread out: each row on its own, every track keeps its distance");
  const auto again = st::spreadOut(t, 0.1);
  bool same = true;
  for (size_t i = 0; i < s.size(); ++i) same = same && s[i].pan == again[i].pan && s[i].depth == again[i].depth;
  expect(same, "spread out is deterministic");
  std::vector<st::Pos> crowd;
  for (int k = 0; k < 30; ++k) crowd.push_back({static_cast<ObjectId>(k + 1), 0, 0});
  const auto c = st::spreadOut(crowd, 0.12);
  int back = 0;
  for (const auto& p : c) back += p.depth > 0;
  expect(back == 15, "a crowded row: every other track a step back");
  // Arrange by role: deterministic, unknowns in the middle, no two on one point.
  using R = st::Role;
  const std::vector<R> roles = {R::Kick, R::Bass, R::LeadVocal, R::Unknown, R::Hats, R::Hats, R::Hats, R::Pad, R::Unknown};
  const auto a = st::arrangeByRole(roles), b = st::arrangeByRole(roles);
  bool det = true, apart = true;
  for (size_t i = 0; i < a.size(); ++i) det = det && a[i].pan == b[i].pan && a[i].depth == b[i].depth;
  for (size_t i = 0; i < a.size(); ++i)
    for (size_t j = i + 1; j < a.size(); ++j) apart = apart && !(near(a[i].pan, a[j].pan, 0.03) && near(a[i].depth, a[j].depth, 0.03));
  expect(det && apart, "arrange by sound: deterministic, never two tracks on one point");
  expect(near(a[2].pan, 0) && a[2].depth == 0 && near(a[3].pan, 0) && near(a[3].depth, 0.5), "lead vocal centre front; an unsure track in the middle");
  expect(a[7].depth >= 0.75 && a[0].depth == 0.25, "pads at the back, kick near");
}

// Synthetic parts: what the analysis hears.
std::vector<float> part(const char* what, double sr, double seconds) {
  std::vector<float> x(static_cast<size_t>(sr * seconds), 0.0f);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> U(-1, 1);
  const std::string w = what;
  float hp = 0, prev = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / sr, beat = std::fmod(t, 0.5);
    if (w == "kick") x[i] = static_cast<float>(0.9 * std::exp(-beat * 18) * std::sin(2 * M_PI * (55 + 80 * std::exp(-beat * 40)) * beat));
    else if (w == "bass") x[i] = static_cast<float>(0.6 * std::sin(2 * M_PI * 65 * t));
    else if (w == "hats") {
      const float n = U(rng);
      hp = 0.85f * (hp + n - prev);
      prev = n;
      x[i] = static_cast<float>(0.5 * std::exp(-std::fmod(t, 0.25) * 60)) * hp;
    } else if (w == "pad") x[i] = static_cast<float>(0.3 * (std::sin(2 * M_PI * 220 * t) + 0.5 * std::sin(2 * M_PI * 330 * t)));
  }
  return x;
}

void testRoles() {
  const double sr = 48000;
  auto feat = [&](const char* w) {
    st::FeatureAcc acc;
    acc.reset(sr);
    const auto x = part(w, sr, 4.0);
    for (size_t i = 0; i < x.size(); i += 512) acc.add(x.data() + i, static_cast<int>(std::min<size_t>(512, x.size() - i)));
    return acc.result();
  };
  std::string why;
  const st::Features fk = feat("kick"), fb = feat("bass"), fh = feat("hats"), fp = feat("pad");
  std::printf("kick c %.0f low %.2f on %.2f sus %.2f | bass c %.0f low %.2f sus %.2f | hats c %.0f sus %.2f | pad c %.0f sus %.2f on %.2f\n", fk.centroid,
              fk.lowShare, fk.onsets, fk.sustain, fb.centroid, fb.lowShare, fb.sustain, fh.centroid, fh.sustain, fp.centroid, fp.sustain, fp.onsets);
  expect(st::roleFromSound(fk, &why) == st::Role::Kick, "an unnamed track's sound: low and punchy is a kick");
  expect(st::roleFromSound(fb, &why) == st::Role::Bass && why == "low and steady", "low and steady is a bass");
  expect(st::roleFromSound(fh, &why) == st::Role::Hats, "bright and short is hats");
  expect(st::roleFromSound(fp, &why) == st::Role::Pad, "long and soft is a pad");
  st::FeatureAcc silent;
  silent.reset(sr);
  std::vector<float> z(48000, 0.0f);
  silent.add(z.data(), 48000);
  const st::Guess g = st::guessRole({}, {}, nullptr, "Audio 3");
  st::Features fs = silent.result();
  expect(st::roleFromSound(fs, &why) == st::Role::Unknown && g.role == st::Role::Unknown && g.source == 0, "silent and unnamed: ? (unsure)");
  expect(st::guessRole({"gl.kick"}, {}, &fb, "Lead Vox").role == st::Role::Kick, "what plays wins over the sound and the name");
  expect(st::guessRole({}, {}, &fb, "Lead Vox").role == st::Role::Bass, "the sound wins over the name");
  expect(st::guessRole({}, {}, nullptr, "Lead Vox").role == st::Role::LeadVocal && st::guessRole({}, {}, nullptr, "Lead Vox").source == 3, "the name is the last hint");
  expect(st::roleFromPlays({"gl.kick", "gl.snare"}, {}) == st::Role::Drums && st::roleFromPlays({"gl.sampler"}, {"/samples/Kick_01.wav"}) == st::Role::Kick,
         "several drum voices: drums; a sampler's file name says what it plays");
}

// ---------------------------------------------------------------- parameters and files
void testParams() {
  Document d(makeEmptySong());
  const ObjectId i1 = d.song().mixer[1].id;
  const auto dd = describeParam(d.song(), mixerParam(i1, MixerParam::Depth));
  const auto fd = describeParam(d.song(), mixerParam(i1, MixerParam::Focus));
  expect(dd && dd->name == "Distance" && dd->min == 0 && dd->max == 1 && fd && fd->name == "Focus" && fd->min == -1 && fd->max == 1 && fd->def == 0,
         "Distance and Focus: names, ranges, defaults");
  const auto list = listParams(d.song());
  expect(std::find(list.begin(), list.end(), mixerParam(i1, MixerParam::Focus)) != list.end(), "Focus is listed (automatable)");
  d.setParam(mixerParam(i1, MixerParam::Focus), 0.6, false);
  d.endGesture();
  expect(near(d.song().mixer[1].focus, 0.6, 1e-6), "Focus sets the track");
  d.undo();
  expect(d.song().mixer[1].focus == 0, "Focus undoes");
  // Save / load of every new field; old files read the defaults; the early flag migrates.
  Song s = makeEmptySong();
  s.mixer[1].depth = 0.5f, s.mixer[1].focus = -0.4f;
  s.panLaw = PanLaw::ConstantPower;
  s.stage.room = s.mixer[3].id, s.stage.depthStrength = 0.9f, s.stage.roomType = 3, s.stage.early = false, s.stage.frontQuiet = false;
  s.mixer[2].sends.push_back({s.mixer[3].id, 0.25f, false, false, true});
  const std::string text = projectToText(s);
  Song back;
  expect(loadProjectText(text, "", back) && back.mixer[1].focus == s.mixer[1].focus && back.panLaw == PanLaw::ConstantPower && back.stage == s.stage &&
             back.mixer[2].sends.back().stageRoom,
         "save / load: focus, pan law, the room settings and the room send");
  const std::string plain = projectToText(makeEmptySong());
  expect(plain.find("\"focus\"") == std::string::npos && plain.find("\"panLaw\"") == std::string::npos && plain.find("\"stage\"") == std::string::npos &&
             plain.find("stageRoom") == std::string::npos,
         "untouched projects write none of it (older versions read the same file)");
  Song old;
  expect(loadProjectText(plain, "", old) && old.panLaw == PanLaw::Balance && old.stage.isDefault() && old.mixer[1].focus == 0, "old files: the old law, no room");
  Json j = songToJson(s);
  for (auto& [k, v] : j.object)
    if (k == "mixer" && v.is(Json::Type::Array) && v.array.size() > 2) {
      v.array[1].object["focus"] = Json::makeNumber(9.0);
      for (auto& sd : v.array[2].object["sends"].array) {
        sd.object.erase("stageRoom");
        { Json t; t.type = Json::Type::Bool; t.boolean = true; sd.object["depthScaled"] = t; }
      }
    }
  Song clamped;
  expect(songFromJson(j, clamped) && clamped.mixer[1].focus == 1.0f && clamped.mixer[2].sends.back().stageRoom, "focus clamps; an early depth send loads as a room send");
  // Reset brings both back, one step.
  d.setParam(mixerParam(i1, MixerParam::Depth), 0.7, false);
  d.setParam(mixerParam(i1, MixerParam::Focus), 0.7, false);
  d.endGesture();
  const size_t at = d.history().position();
  expect(mixops::resetParams(d, {i1}, {MixerParam::Depth, MixerParam::Focus}) > 0 && d.song().mixer[1].depth == 0 && d.song().mixer[1].focus == 0 &&
             d.history().position() == at + 1,
         "reset distance and focus: one step");
}

void testRoomOps() {
  Document d(makeEmptySong());
  const ObjectId m = d.song().masterId(), t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id;
  const size_t before = d.history().position();
  const ObjectId room = mixops::ensureStageRoom(d, {t1, t2, m});
  const MixerTrack* r = d.song().mixerTrack(room);
  expect(r && r->name == "Room" && r->slots[0].plugin.type == "gl.reverb" && d.song().stage.room == room, "the room: an ordinary track with a reverb, marked as the room");
  bool sends = true, masterSends = false;
  for (ObjectId id : {t1, t2}) {
    bool ok = false;
    for (const auto& sd : d.song().mixerTrack(id)->sends) ok = ok || (sd.to == room && sd.stageRoom && near(sd.level, mixops::kRoomSendLevel, 1e-6));
    sends = sends && ok;
  }
  for (const auto& sd : d.song().mixer[0].sends) masterSends = masterSends || sd.to == room;
  expect(sends && !masterSends, "room sends at -12 dB from the tracks, never from the master");
  expect(d.history().position() == before + 1, "making the room: one undo step");
  const size_t tracks = d.song().mixer.size();
  expect(mixops::ensureStageRoom(d, {t1}) == room && d.song().mixer.size() == tracks, "a second call reuses the room (never a surprise)");
  const size_t at = d.history().position();
  expect(mixops::setRoomType(d, 4) && d.song().stage.roomType == 4 && d.history().position() == at + 1, "room type: one step");
  expect(mixops::setRoomOn(d, false) && d.song().mixerTrack(room)->muted && d.song().mixerTrack(room), "room off mutes it, deletes nothing");
  expect(mixops::setPanLaw(d, PanLaw::Stage) && d.song().panLaw == PanLaw::Stage, "pan law: set");
  d.undo();
  expect(d.song().panLaw == PanLaw::Balance, "pan law: undone");
  while (d.history().position() > before) d.undo();
  expect(!d.song().mixerTrack(room) && d.song().stage.room == kNoId, "the room undoes whole");
}

// ---------------------------------------------------------------- the engine
// A steady sine (param 0 Hz; param 1: 1 = the right channel inverted, 2 = an impulse instead).
class TestSine final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double f = s.params.empty() ? 100.0 : s.params[0];
    const float mode = s.params.size() > 1 ? s.params[1] : 0.0f;
    for (int i = 0; i < n; ++i, ++t_) {
      float v = static_cast<float>(0.25 * std::sin(2.0 * M_PI * f * static_cast<double>(t_) / 44100.0));
      if (mode > 1.5f) v = t_ == 0 ? 1.0f : 0.0f;
      L[i] = v;
      R[i] = mode > 0.5f && mode < 1.5f ? -v : v;
    }
  }
  void reset() override { t_ = 0; }

 private:
  long t_ = 0;
};
std::unique_ptr<Effect> testFactory(const std::string& type) {
  if (type == "test.sine") return std::make_unique<TestSine>();
  return nullptr;
}

Song sineSong(double hz, float depth, float mode = 0) {
  Song s = makeEmptySong();
  s.mixer[1].slots[0].plugin.type = "test.sine";
  s.mixer[1].slots[0].plugin.params = {static_cast<float>(hz), mode};
  s.mixer[1].depth = depth;
  s.stage.roomType = static_cast<uint8_t>(rooms::kStudio);  // distance is heard in a room
  return s;
}

struct Out {
  std::vector<float> L, R;
};
Out run(const Song& s, int frames, int block = 256) {
  ProgramBuilder b;
  AudioEngine e(44100);
  b.setSampleRate(44100);
  e.setThreads(1);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Pattern);
  if (!s.patterns.empty()) e.setPattern(s.patterns[0].id);
  e.play();
  Out o;
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  for (int done = 0; done < frames; done += block) {
    const int n = std::min(block, frames - done);
    e.process(L.data(), R.data(), n);
    o.L.insert(o.L.end(), L.begin(), L.begin() + n);
    o.R.insert(o.R.end(), R.begin(), R.begin() + n);
  }
  return o;
}
double rms(const std::vector<float>& x, size_t from) {
  double a = 0;
  for (size_t i = from; i < x.size(); ++i) a += static_cast<double>(x[i]) * x[i];
  return std::sqrt(a / static_cast<double>(std::max<size_t>(1, x.size() - from)));
}
double db(double r, double ref) { return 20.0 * std::log10(std::max(1e-12, r) / ref); }
bool same(const Out& a, const Out& b) {
  return a.L.size() == b.L.size() && std::memcmp(a.L.data(), b.L.data(), a.L.size() * 4) == 0 && std::memcmp(a.R.data(), b.R.data(), a.R.size() * 4) == 0;
}

void testBitExact() {
  const int N = 22050;
  Song a = sineSong(1000, 0, 1);
  a.stage.roomType = 0;
  a.mixer[1].pan = -0.4f, a.mixer[1].stereoSep = 0.3f, a.mixer[1].volume = 0.7f;
  const Out x = run(a, N);
  // The same project through a file without any Stage field (as an older version wrote it).
  Song old;
  expect(loadProjectText(projectToText(a), "", old), "reload");
  old.mixer[1].slots[0].plugin = a.mixer[1].slots[0].plugin;
  expect(same(run(old, N), x), "an old project renders bit for bit as before (distance 0, focus 0, Balance, no room)");
  // The Balance law is the strip's arithmetic from before the Stage, sample for sample.
  float worst = 0;
  {
    Song plain = sineSong(1000, 0, 0);
    plain.mixer[1].pan = -0.4f, plain.mixer[1].volume = 0.7f;
    const Out o = run(plain, N);
    double l = 0, r = 0;
    dsp::panGains(-0.4, l, r);
    const double g = dsp::faderGain(0.7, 0.8);
    for (size_t i = 1000; i < o.L.size(); ++i) {
      const float v = static_cast<float>(0.25 * std::sin(2.0 * M_PI * 1000 * static_cast<double>(i) / 44100.0));
      worst = std::max({worst, std::fabs(o.L[i] - v * static_cast<float>(g * l)), std::fabs(o.R[i] - v * static_cast<float>(g * r))});
    }
  }
  expect(worst < 1e-6f, "Balance law: the old strip gains exactly");
  // Moving only left / right on the Stage is the pan knob: the same parameter, the same render.
  Document doc(sineSong(1000, 0));
  doc.setParam(mixerParam(doc.song().mixer[1].id, MixerParam::Pan), 0.35, false);
  Song knob = sineSong(1000, 0);
  knob.mixer[1].pan = 0.35f;
  expect(same(run(doc.song(), N), run(knob, N)), "a Stage move left / right equals turning the pan knob");
}

void testLaws() {
  const int N = 44100;
  const double ref = rms(run(sineSong(500, 0), N).L, N / 2);
  // Constant power: L^2 + R^2 constant over a pan sweep, centre unity.
  double lo = 1e9, hi = -1e9;
  for (double p = -1; p <= 1.0001; p += 0.25) {
    Song s = sineSong(500, 0);
    s.panLaw = PanLaw::ConstantPower;
    s.mixer[1].pan = static_cast<float>(p);
    const Out o = run(s, N);
    const double l = rms(o.L, N / 2), r = rms(o.R, N / 2);
    const double pw = 10 * std::log10((l * l + r * r) / (2 * ref * ref));
    lo = std::min(lo, pw), hi = std::max(hi, pw);
    if (p == 0) expect(near(db(l, ref), 0, 0.01), "constant power: centre is unity");
  }
  expect(hi - lo < 0.01, "constant power: L^2 + R^2 constant over the pan sweep (within 0.01 dB)");
  // Stage placement: a stereo track panned hard left keeps both channels; Balance loses one.
  Song st2 = sineSong(500, 0, 1);
  st2.panLaw = PanLaw::Stage;
  st2.mixer[1].pan = -1;
  Song bal = st2;
  bal.panLaw = PanLaw::Balance;
  const Out os = run(st2, N), ob = run(bal, N);
  std::printf("hard left: stage L %.3f R %.3f | balance L %.3f R %.3f\n", rms(os.L, N / 2), rms(os.R, N / 2), rms(ob.L, N / 2), rms(ob.R, N / 2));
  expect(rms(os.R, N / 2) > 0.5 * ref && rms(ob.R, N / 2) < 1e-6, "Stage placement: a stereo track hard left keeps its right channel");
  Song centre = sineSong(500, 0, 1);
  centre.panLaw = PanLaw::Stage;
  Song centreB = centre;
  centreB.panLaw = PanLaw::Balance;
  expect(same(run(centre, N / 4), run(centreB, N / 4)), "Stage placement at the centre, as recorded: exactly the input");
  // Mono width under Stage placement: the constant power law of a mono source.
  Song mono = sineSong(500, 0);
  mono.panLaw = PanLaw::Stage, mono.mixer[1].stereoSep = -1, mono.mixer[1].pan = 0;
  expect(near(db(rms(run(mono, N).L, N / 2), ref), 0, 0.05), "Stage placement: a mono track at the centre stays at unity");
}

void testDistanceAndFocus() {
  const int N = 44100;
  const double lo0 = rms(run(sineSong(100, 0), N).L, N / 2), hi0 = rms(run(sineSong(10000, 0), N).L, N / 2);
  const double lo1 = rms(run(sineSong(100, 1), N).L, N / 2), hi1 = rms(run(sineSong(10000, 1), N).L, N / 2);
  std::printf("distance 1 (natural): 100 Hz %.2f dB, 10 kHz %.2f dB\n", db(lo1, lo0), db(hi1, hi0));
  expect(near(db(lo1, lo0), -9.03, 0.1), "distance 1, natural: a low tone 9 dB down");
  expect(near(db(hi1, hi0), -18.0, 1.0), "distance 1: 10 kHz about 18 dB down (the air shelf)");
  for (float strength : {0.0f, 1.0f}) {
    Song s = sineSong(100, 1);
    s.stage.depthStrength = strength;
    const double v = db(rms(run(s, N).L, N / 2), lo0);
    expect(near(v, strength == 0 ? -6.02 : -12.04, 0.1), strength == 0 ? "gentle: 6 dB at the back" : "strong: 12 dB at the back");
  }
  {
    const double s0 = rms(run(sineSong(100, 0, 1), N).L, N / 2), s1 = rms(run(sineSong(100, 1, 1), N).L, N / 2);
    expect(near(s1 / s0, std::pow(4.0, -0.75) * 0.65, 0.01), "distance 1: the side narrowed to 65 %");
  }
  {
    Song f = sineSong(2800, 0);
    f.mixer[1].focus = 1;
    Song l = sineSong(100, 0);
    l.mixer[1].focus = 1;
    const double p = db(rms(run(f, N).L, N / 2), rms(run(sineSong(2800, 0), N).L, N / 2));
    const double q = db(rms(run(l, N).L, N / 2), lo0);
    std::printf("focus +1: 2.8 kHz %+.2f dB, 100 Hz %+.2f dB\n", p, q);
    expect(near(p, 3.0, 0.1) && std::fabs(q) < 0.3, "focus +1 (present): a 3 dB presence lift, the lows alone");
    f.mixer[1].focus = -1;
    expect(near(db(rms(run(f, N).L, N / 2), rms(run(sineSong(2800, 0), N).L, N / 2)), -3.0, 0.1), "focus -1 (soft): a 3 dB presence dip");
  }
  // Block-size invariance with every part running.
  {
    Song s = sineSong(3000, 0.6f, 1);
    s.mixer[1].focus = 0.5f, s.mixer[1].pan = -0.3f;
    s.panLaw = PanLaw::Stage;
    const Out ref = run(s, 8192, 512);
    bool ok = true;
    for (int blk : {1, 17, 64}) {
      const Out o = run(s, 8192, blk);
      float diff = 0;
      for (size_t i = 0; i < o.L.size(); ++i) diff = std::max({diff, std::fabs(o.L[i] - ref.L[i]), std::fabs(o.R[i] - ref.R[i])});
      ok = ok && diff < 1e-5f;
    }
    expect(ok, "the stage: the same output at block sizes 1, 17, 64 and 512");
  }
  // Extremes stay finite; silence stays silence.
  {
    bool fin = true;
    for (float d : {0.0f, 1e-7f, 1.0f})
      for (float f : {-1.0f, 1.0f}) {
        Song x = sineSong(15000, d, 1);
        x.mixer[1].focus = f, x.mixer[1].stereoSep = 1, x.mixer[1].volume = 1.25f, x.panLaw = PanLaw::Stage, x.mixer[1].pan = 1;
        for (float v : run(x, 4096).L) fin = fin && std::isfinite(v);
      }
    Song z = makeEmptySong();
    z.mixer[1].depth = 1, z.mixer[1].focus = 1;
    float pk = 0;
    for (float v : run(z, 4096).L) pk = std::max(pk, std::fabs(v));
    expect(fin && pk == 0.0f, "extremes finite; silence in, silence out");
  }
}

// Track 1 plays; its only route is the room (track 2, no effects), so the output is the room's input.
Song roomSong(double hz, float depth, float mode, bool frontQuiet, bool preDelay, bool early, bool dryToo = false) {
  Song s = sineSong(hz, depth, mode);
  s.stage.room = s.mixer[2].id;
  s.stage.frontQuiet = frontQuiet, s.stage.preDelay = preDelay, s.stage.early = early;
  s.mixer[1].sends.clear();
  if (dryToo) s.mixer[1].sends.push_back({s.masterId(), 1.0f});
  s.mixer[1].sends.push_back({s.mixer[2].id, 1.0f, false, false, true});
  return s;
}

void testRoom() {
  const int N = 44100;
  // Wet level does not fall with distance (front factor off).
  const double w0 = rms(run(roomSong(100, 0, 0, false, true, false), N).L, N / 2), w1 = rms(run(roomSong(100, 1, 0, false, true, false), N).L, N / 2);
  std::printf("room: wet at the back vs front %+.3f dB\n", db(w1, w0));
  expect(near(db(w1, w0), 0, 0.1), "the room send keeps its level with distance (front factor off)");
  // With the front factor the front row sends 6 dB less; the dry-to-room ratio falls about 15 dB.
  const double q0 = rms(run(roomSong(100, 0, 0, true, true, false), N).L, N / 2), q1 = rms(run(roomSong(100, 1, 0, true, true, false), N).L, N / 2);
  expect(near(db(q1, q0), 6.02, 0.1), "front factor: the front row 6 dB quieter in the room");
  // Pre-delay: 2 + 18 (1 - d) ms, to the sample.
  bool exact = true;
  for (float d : {0.0f, 0.5f, 1.0f}) {
    const Out o = run(roomSong(100, d, 2, false, true, false), 4096, 64);
    int first = -1;
    for (size_t i = 0; i < o.L.size() && first < 0; ++i)
      if (std::fabs(o.L[i]) > 1e-6f) first = static_cast<int>(i);
    const int want = static_cast<int>(std::lround(depth::preDelaySec(d) * 44100));
    std::printf("pre-delay at %.1f: %d (want %d)\n", static_cast<double>(d), first, want);
    exact = exact && first == want;
  }
  expect(exact, "pre-delay follows distance: 20 ms at the front, 2 ms at the back, to the sample");
  // Early reflections: a hard-left track's left wall tap is louder on the left.
  {
    Song s = roomSong(100, 0.3f, 2, false, true, true);
    s.mixer[1].pan = -1;
    s.mixer[2].pan = 0;
    const Out o = run(s, 8192, 64);
    depth::EarlyTap taps[3];
    depth::earlyTaps(-1, 0.3, taps);
    const size_t at = static_cast<size_t>(std::lround(taps[0].delaySec * 44100));
    bool fin = true;
    for (float v : o.L) fin = fin && std::isfinite(v);
    std::printf("left wall tap at %zu: L %.4f R %.4f\n", at, static_cast<double>(o.L[at]), static_cast<double>(o.R[at]));
    expect(fin && at < o.L.size() && o.L[at] > o.R[at] && o.R[at] > 0, "early reflections: finite, the left wall's tap on the left");
  }
  // Room off: the room track muted, the room silent.
  {
    Document d(roomSong(100, 0.5f, 0, true, true, true));
    expect(mixops::setRoomOn(d, false), "room off");
    float pk = 0;
    for (float v : run(d.song(), 8192).L) pk = std::max(pk, std::fabs(v));
    expect(pk == 0.0f, "room off: nothing from the room");
  }
  // Block-size invariance with the room running.
  {
    const Song s = roomSong(700, 0.4f, 1, true, true, true, true);
    const Out ref = run(s, 8192, 512);
    bool ok = true;
    for (int blk : {1, 17, 64}) {
      const Out o = run(s, 8192, blk);
      float diff = 0;
      for (size_t i = 0; i < o.L.size(); ++i) diff = std::max({diff, std::fabs(o.L[i] - ref.L[i]), std::fabs(o.R[i] - ref.R[i])});
      ok = ok && diff < 1e-5f;
    }
    expect(ok, "the room: the same output at block sizes 1, 17, 64 and 512");
  }
}

// "No room" (the default): distance and focus are kept on the tracks but change nothing in the sound.
// Each room renders finite and distinct; picking rooms and going back to no room is one undo step each.
void testRooms() {
  const int N = 22050;
  {
    Song a = sineSong(3000, 0, 1), b = sineSong(3000, 1, 1);
    a.stage.roomType = 0, b.stage.roomType = 0;
    b.mixer[1].focus = 1;
    a.mixer[1].pan = b.mixer[1].pan = 0.3f;
    expect(same(run(a, N), run(b, N)), "no room: distance 1 and focus +1 render bit for bit as distance 0, focus 0");
    Song r0 = roomSong(500, 0, 0, true, true, true, true), r1 = roomSong(500, 1, 0, true, true, true, true);
    r0.stage.roomType = 0, r1.stage.roomType = 0;
    r1.mixer[1].focus = -1;
    expect(same(run(r0, N), run(r1, N)), "no room: room sends, pre-delay and reflections ignore distance and focus too");
    Song w = sineSong(3000, 0, 1), w2 = sineSong(3000, 0, 1);
    w.stage.roomType = w2.stage.roomType = 0;
    w2.mixer[1].stereoSep = -0.6f, w2.mixer[1].pan = -0.5f;
    expect(!same(run(w, N), run(w2, N)), "no room: pan and width still work");
  }
  // Each room, from a project with no room: a Room track appears, the render is finite and its own.
  std::vector<double> prints;
  bool finite = true, made = true, steps = true;
  for (int k = 1; k < rooms::kCount; ++k) {
    Song s0 = sineSong(0, 0.6f, 2);  // an impulse at 60 % depth
    s0.stage.roomType = 0;
    s0.mixer[1].pan = -0.4f;
    Document d(s0);
    const size_t at = d.history().position();
    const size_t tracks = d.song().mixer.size();
    made = made && mixops::setRoomType(d, k, {d.song().mixer[1].id}) && d.song().stage.roomType == k && mixops::stageRoom(d.song()) &&
           d.song().mixer.size() == tracks + 1;
    steps = steps && d.history().position() == at + 1;
    const Out o = run(d.song(), 44100 * 2, 512);
    double e = 0, late = 0;
    for (size_t i = 0; i < o.L.size(); ++i) {
      finite = finite && std::isfinite(o.L[i]) && std::isfinite(o.R[i]);
      e += static_cast<double>(o.L[i]) * o.L[i] + static_cast<double>(o.R[i]) * o.R[i];
      if (i > 44100 / 2) late += static_cast<double>(o.L[i]) * o.L[i];
    }
    std::printf("room %-10s energy %.5f  tail after 0.5 s %.6f\n", rooms::key(k), e, late);
    prints.push_back(e * 1000.0 + late);
  }
  bool distinct = true;
  for (size_t i = 0; i < prints.size(); ++i)
    for (size_t j = i + 1; j < prints.size(); ++j) distinct = distinct && std::fabs(prints[i] - prints[j]) > 1e-6 * std::max(1.0, prints[i]);
  expect(made && steps, "every room: picked in one step, the Room track made then");
  expect(finite && distinct, "every room renders finite and distinct");
  // Switching rooms and back to no room: one step each, the room muted (never deleted), undo brings it back.
  {
    Document d(sineSong(100, 0.5f));
    d.editSong("x", [](Song& s) { s.stage.roomType = 0; });
    d.endGesture();
    const ObjectId t1 = d.song().mixer[1].id;
    const size_t start = d.history().position();
    expect(mixops::setRoomType(d, rooms::kHall, {t1}) && d.history().position() == start + 1, "pick the hall: one step");
    const ObjectId room = d.song().stage.room;
    const float hallSend = mixops::sendGain(d.song(), t1, room) > 0 ? mixops::roomSendLevel(d.song()) : 0;
    expect(mixops::setRoomType(d, rooms::kSmall, {t1}) && d.history().position() == start + 2 && d.song().stage.roomType == rooms::kSmall,
           "switch to the small room: one step");
    bool follows = false;
    for (const auto& sd : d.song().mixerTrack(t1)->sends)
      if (sd.to == room) follows = near(sd.level, mixops::roomSendLevel(d.song()), 1e-6) && !near(sd.level, hallSend, 1e-6);
    expect(follows, "an untouched room send follows the room's own level");
    expect(mixops::setRoomType(d, 0) && d.history().position() == start + 3 && d.song().mixerTrack(room) && d.song().mixerTrack(room)->muted &&
               !d.song().stage.hasRoom(),
           "no room: one step, the Room track muted, never deleted");
    d.undo();
    expect(d.song().stage.roomType == rooms::kSmall && !d.song().mixerTrack(room)->muted, "undo: the small room again, heard");
    d.undo();
    expect(d.song().stage.roomType == rooms::kHall, "undo: the hall");
    d.undo();
    expect(!d.song().stage.hasRoom() && !d.song().mixerTrack(room), "undo: no room and no Room track");
    d.redo();
    expect(d.song().stage.roomType == rooms::kHall && d.song().mixerTrack(room), "redo: the hall and its track");
  }
  // Loading: a new project has no room; an early Stage project gets the room it was made for.
  {
    expect(!makeEmptySong().stage.hasRoom(), "a new project: no room");
    Song e = makeEmptySong();
    e.mixer[1].depth = 0.5f;
    Json j = songToJson(e);
    j.object.erase("stage");
    Song back;
    expect(songFromJson(j, back) && back.stage.roomType == rooms::kStudio, "an early project with distance but no room loads in the studio");
    Json st = Json::makeObject();
    st.object["roomType"] = Json::makeNumber(2);
    j.object["stage"] = st;
    expect(songFromJson(j, back) && back.stage.roomType == rooms::kHall, "an early room type number loads as the same room");
    Song plain;
    expect(songFromJson(songToJson(makeEmptySong()), plain) && !plain.stage.hasRoom(), "a project that never used the Stage stays without a room");
    Song saved = makeEmptySong();
    saved.stage.roomType = rooms::kPlate;
    Song again;
    expect(loadProjectText(projectToText(saved), "", again) && again.stage.roomType == rooms::kPlate, "a room saves and loads by name");
  }
}

void testAutomation() {
  Song s = sineSong(100, 0);
  s.stage.room = s.mixer[2].id;
  s.mixer[1].sends.push_back({s.mixer[2].id, 0.5f, false, false, true});
  for (MixerParam p : {MixerParam::Depth, MixerParam::Focus}) {
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = s.mixer[1].id;
    ac.index = static_cast<uint32_t>(p);
    ac.points = {{0, p == MixerParam::Depth ? 0.0f : 0.5f, 0.0f, AutoShape::Curve}, {s.barTicks(), 1.0f, 0.0f, AutoShape::Curve}};
    s.automations.push_back(ac);
    Clip c;
    c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = ac.id, c.track = static_cast<int>(s.automations.size()) - 1, c.start = 0,
    c.length = s.barTicks() * 4;
    s.playlist.push_back(c);
  }
  ProgramBuilder b;
  AudioEngine e(44100);
  b.setSampleRate(44100);
  e.setThreads(1);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Song);
  e.play();
  std::vector<float> L(256), R(256), all;
  for (int k = 0; k < 44100 * 2 / 256; ++k) {
    e.process(L.data(), R.data(), 256);
    all.insert(all.end(), L.begin(), L.end());
  }
  double live = -1, liveF = -1;
  expect(b.modulatedValue(mixerParam(s.mixer[1].id, MixerParam::Depth), live) && live > 0.5 && b.modulatedValue(mixerParam(s.mixer[1].id, MixerParam::Focus), liveF) &&
             liveF > 0,
         "automation: the live distance and focus are readable (the Stage follows them)");
  float jump = 0;
  for (size_t i = 1; i < all.size(); ++i) jump = std::max(jump, std::fabs(all[i] - all[i - 1]));
  expect(jump < 0.02f, "automating distance and focus with a room: no clicks");
}

// ---------------------------------------------------------------- undo of every Stage gesture
void testUndoSteps() {
  Document d(makeEmptySong());
  const ObjectId a = d.song().mixer[1].id, b = d.song().mixer[2].id, c = d.song().mixer[3].id;
  auto step = [&](const char* what, const std::function<void()>& fn) {
    d.endGesture();
    const size_t at = d.history().position();
    fn();
    d.endGesture();
    const bool one = d.history().position() == at + 1;
    const Song after = d.song();
    d.undo();
    const bool back = d.history().position() == at;
    d.redo();
    expect(one && back && d.song() == after, what);
  };
  // A drag (pointer gesture): move two tracks.
  step("a drag on the stage: one step, undo and redo", [&] {
    d.beginPointerGesture();
    for (int k = 1; k <= 8; ++k) {
      d.setParam(mixerParam(a, MixerParam::Pan), -0.05 * k, false);
      d.setParam(mixerParam(a, MixerParam::Depth), 0.05 * k, false);
      d.setParam(mixerParam(b, MixerParam::Pan), 0.05 * k, false);
    }
    d.endPointerGesture();
  });
  // Join a group: take the target's exact spot.
  step("joining a group: one step", [&] {
    d.beginGroup("join group");
    d.setParam(mixerParam(c, MixerParam::Pan), d.song().mixer[1].pan, false);
    d.setParam(mixerParam(c, MixerParam::Depth), d.song().mixer[1].depth, false);
    d.endGroup();
  });
  expect(d.song().mixer[3].pan == d.song().mixer[1].pan && d.song().mixer[3].depth == d.song().mixer[1].depth, "the joined track shares the spot exactly");
  {
    std::vector<st::Pos> p;
    for (size_t i = 1; i <= 3; ++i) p.push_back({d.song().mixer[i].id, d.song().mixer[i].pan, d.song().mixer[i].depth});
    expect(st::groups(p).size() == 1 && st::groups(p)[0].size() == 2, "they form a group of two");
    // Drag out: the group of two dissolves.
    d.beginGroup("move out of the group");
    d.setParam(mixerParam(c, MixerParam::Pan), 0.6, false);
    d.endGroup();
    p.clear();
    for (size_t i = 1; i <= 3; ++i) p.push_back({d.song().mixer[i].id, d.song().mixer[i].pan, d.song().mixer[i].depth});
    expect(st::groups(p).empty(), "dragging a member out dissolves a group of two");
  }
  step("width handles: one step", [&] {
    d.beginPointerGesture();
    for (int k = 1; k <= 5; ++k) d.setParam(mixerParam(a, MixerParam::StereoSep), -0.1 * k, false);
    d.endPointerGesture();
  });
  step("Spread out: one step", [&] {
    std::vector<st::Pos> p;
    for (size_t i = 1; i <= 4; ++i) p.push_back({d.song().mixer[i].id, d.song().mixer[i].pan, d.song().mixer[i].depth});
    const auto to = st::spreadOut(p, 0.1);
    d.beginGroup("spread out");
    for (const auto& q : to) {
      d.setParam(mixerParam(q.id, MixerParam::Pan), q.pan, false);
      d.setParam(mixerParam(q.id, MixerParam::Depth), q.depth, false);
    }
    d.endGroup();
  });
  step("the room on the stage: one step", [&] { mixops::ensureStageRoom(d, {a, b}); });
  step("depth strength: one step", [&] { mixops::setDepthStrength(d, 0.9, false); });
  step("room size: one step", [&] {
    d.beginGroup("room size");
    mixops::setRoomSize(d, 0.8, false);
    d.endGroup();
  });
}

// ---------------------------------------------------------------- collaborators
void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(188);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  a.doc.reset(makeEmptySong());
  a.folder = tmpDir("k188_a"), b.folder = tmpDir("k188_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  const ObjectId t1 = a.doc.song().mixer[1].id, t2 = a.doc.song().mixer[2].id;
  a.doc.beginPointerGesture();
  a.doc.setParam(mixerParam(t1, MixerParam::Pan), -0.5, false);
  a.doc.setParam(mixerParam(t1, MixerParam::Depth), 0.7, false);
  a.doc.setParam(mixerParam(t1, MixerParam::Focus), 0.4, false);
  a.doc.endPointerGesture();
  a.doc.endGesture();
  settle(net, {&a, &b}, clock);
  expect(songText(a) == songText(b) && near(b.doc.song().mixer[1].depth, 0.7, 1e-6) && near(b.doc.song().mixer[1].focus, 0.4, 1e-6),
         "collab: a move on the stage reaches the other side");
  const ObjectId room = mixops::ensureStageRoom(b.doc, {t2});
  mixops::setPanLaw(b.doc, PanLaw::Stage);
  mixops::setDepthStrength(b.doc, 0.8, false);
  settle(net, {&a, &b}, clock);
  expect(room != kNoId && a.doc.song().stage.room == room && a.doc.song().panLaw == PanLaw::Stage && near(a.doc.song().stage.depthStrength, 0.8, 1e-6) &&
             songText(a) == songText(b),
         "collab: the room, the pan law and the depth strength reach the other side");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().mixer[1].depth == 0 && b.doc.song().stage.room == room, "collab: undo takes back only my own move");
}

// ---------------------------------------------------------------- FL export
void testFlpExport() {
  Song s = makeEmptySong();
  s.panLaw = PanLaw::Stage;
  s.mixer[1].depth = 1, s.mixer[1].focus = 0.5f;
  s.mixer[2].depth = 0.5f;
  s.stage.room = s.mixer[3].id;
  s.stage.roomType = static_cast<uint8_t>(rooms::kHall);
  s.mixer[1].sends.push_back({s.mixer[3].id, 0.25f, false, false, true});
  {
    Song none = s;
    none.stage.roomType = 0;
    flp::Bytes o2;
    flp::ExportReport r2;
    expect(flp::exportFlp(none, o2, r2, flp::ExportOptions{}) && r2.counts["mixer: track depth written as level, EQ and width"] == 0 &&
               r2.counts["mixer: track focus left out"] == 0,
           "FL export with no room: distance and focus are not heard, so nothing is baked");
  }
  flp::Bytes out;
  flp::ExportReport rep;
  flp::ExportOptions opt;
  expect(flp::exportFlp(s, out, rep, opt) && !out.empty(), "FL export with the stage: written");
  expect(rep.counts["mixer: track depth written as level, EQ and width"] == 2 && rep.counts["mixer: track focus left out"] == 1 &&
             rep.counts["mixer: the Stage pan law written as plain pans"] == 1,
         "FL export: distance baked, focus and the pan law reported");
  expect(s.mixer[1].depth == 1 && s.mixer[1].sends.back().stageRoom, "FL export leaves the project itself alone");
}

// Stage presets: every preset is deterministic, never puts two tracks on one point, stays on the stage,
// and the roles have icons.
void testPresets() {
  using R = st::Role;
  const std::vector<R> roles = {R::LeadVocal, R::Kick, R::Bass, R::Snare, R::Hats, R::Keys, R::Guitar, R::Pad, R::Pad, R::Fx,
                                R::Strings, R::Brass, R::BackingVocal, R::Unknown, R::Unknown};
  bool same = true, apart = true, inside = true;
  for (int k = 0; k < st::kPresetCount; ++k) {
    const auto a = st::arrangePreset(k, roles), b = st::arrangePreset(k, roles);
    for (size_t i = 0; i < a.size(); ++i) {
      same = same && a[i].pan == b[i].pan && a[i].depth == b[i].depth;
      inside = inside && a[i].pan >= -1 && a[i].pan <= 1 && a[i].depth >= 0 && a[i].depth <= 1;
      for (size_t j = 0; j < i; ++j) apart = apart && !(std::fabs(a[i].pan - a[j].pan) < 0.03 && std::fabs(a[i].depth - a[j].depth) < 0.03);
    }
    expect(st::preset(k).name && *st::preset(k).name && st::preset(k).roomType > 0 && st::preset(k).roomType < rooms::kCount, "preset has a name and a room");
  }
  expect(same, "presets: the same tracks give the same layout");
  expect(apart, "presets: never two tracks on one point");
  expect(inside, "presets: every place is on the stage");
  const auto band = st::arrangePreset(0, roles), lofi = st::arrangePreset(5, roles);
  double wb = 0, wl = 0;
  for (size_t i = 0; i < band.size(); ++i) wb += std::fabs(band[i].pan), wl += std::fabs(lofi[i].pan);
  expect(wl < wb, "Lo-fi room is narrower than Band");
  expect(band[0].pan == 0 && band[0].depth == 0, "Band: the lead vocal stands centre front");
  bool icons = st::roleIcon(R::Unknown) == 0;
  for (int r = 1; r < static_cast<int>(R::Count); ++r) icons = icons && st::roleIcon(static_cast<R>(r)) > 0;
  expect(icons, "every role but unknown has a track icon");
}

}  // namespace

int main() {
  testGeometry();
  testGroups();
  testTray();
  testDrops();
  testLabels();
  testSpread();
  testRoles();
  testPresets();
  testParams();
  testRoomOps();
  setTestEffectFactory(&testFactory);
  testBitExact();
  testLaws();
  testDistanceAndFocus();
  testRoom();
  testAutomation();
  testRooms();
  setTestEffectFactory(nullptr);
  testUndoSteps();
  testCollab();
  testFlpExport();
  return finish();
}
