// Gridline — automation editing (J-5): curve maths per mode, point edit ops with
// undo/redo, LFO tool output, shape presets, FL automation preset import,
// recording thinning.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../src/core/Automation.h"
#include "../src/core/AutomationEdit.h"
#include "../src/core/Document.h"
#include "../src/core/Json.h"
#include "../src/core/Presets.h"
#include "../src/core/Project.h"
#include "../src/core/flp/FlpPresets.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;
namespace ae = gl::autoedit;
namespace fs = std::filesystem;

namespace {
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
constexpr double kPi = 3.14159265358979323846;

AutoPoint pt(Tick pos, float v, float t = 0, AutoShape s = AutoShape::Curve) {
  AutoPoint p;
  p.pos = pos;
  p.value = v;
  p.tension = t;
  p.shape = s;
  return p;
}

// --- synthetic FL bytes ------------------------------------------------------
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void putF32(std::vector<uint8_t>& v, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put32(v, u);
}
void putF64(std::vector<uint8_t>& v, double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  put32(v, static_cast<uint32_t>(u));
  put32(v, static_cast<uint32_t>(u >> 32));
}
void varEvent(std::vector<uint8_t>& ev, uint8_t id, const std::vector<uint8_t>& payload) {
  ev.push_back(id);
  size_t n = payload.size();
  do {
    uint8_t b = n & 0x7F;
    n >>= 7;
    ev.push_back(static_cast<uint8_t>(b | (n ? 0x80 : 0)));
  } while (n);
  ev.insert(ev.end(), payload.begin(), payload.end());
}
struct FlPoint {
  double delta, value;
  float tension;
  uint8_t mode;
};
// Event 234 payload in the given point format (1: 16-byte f32 records, 3: 24-byte f64).
std::vector<uint8_t> points234(const std::vector<FlPoint>& pts, uint32_t format) {
  std::vector<uint8_t> v;
  put32(v, 1);
  put32(v, 0x40);
  v.push_back(1);
  put32(v, 4);
  put32(v, format);
  put32(v, static_cast<uint32_t>(pts.size()));
  for (const auto& p : pts) {
    if (format == 1) {
      putF32(v, static_cast<float>(p.delta));
      putF32(v, static_cast<float>(p.value));
      putF32(v, p.tension);
      v.insert(v.end(), {p.mode, 0, 0, 0});
    } else {
      putF64(v, p.delta);
      putF64(v, p.value);
      putF32(v, p.tension);
      v.insert(v.end(), {p.mode, 0, 0, 0});
    }
  }
  for (int i = 0; i < 25; ++i) put32(v, 0);  // trailer
  return v;
}
std::vector<uint8_t> automationFst(const std::vector<FlPoint>& pts, uint32_t format) {
  std::vector<uint8_t> ev;
  varEvent(ev, 199, {'4', 0, '.', 0, '6', 0, 0, 0});
  ev.insert(ev.end(), {64, 0, 0});  // channel 0
  ev.insert(ev.end(), {21, 5});     // channel type: automation clip
  varEvent(ev, 234, points234(pts, format));
  std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0x20, 0, 1, 0, 96, 0, 'F', 'L', 'd', 't'};
  put32(f, static_cast<uint32_t>(ev.size()));
  f.insert(f.end(), ev.begin(), ev.end());
  return f;
}
void writeBytes(const fs::path& p, const std::vector<uint8_t>& b) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
}  // namespace

int main() {
  gl::test::hermeticTestEnv("autoedit");
  // ---------------------------------------------------------------- curves ----
  {
    bool ends = true, finite = true;
    for (int k = 0; k < static_cast<int>(AutoShape::Count); ++k) {
      AutoShape sh = static_cast<AutoShape>(k);
      for (double t : {-1.0, -0.5, 0.0, 0.3, 1.0}) {
        if (sh != AutoShape::Pulse && sh != AutoShape::Wave)
          ends = ends && near(autoShape(sh, t, 0), 0, 1e-9) && near(autoShape(sh, t, 1), 1, 1e-9);
        for (int i = 0; i <= 50; ++i) {
          double v = autoShape(sh, t, i / 50.0);
          finite = finite && std::isfinite(v) && v >= -1e-9 && v <= 1 + 1e-9;
        }
      }
    }
    expect(ends, "every mode runs from the previous value to the point's value");
    expect(finite, "every mode stays within 0..1 at all tensions");
  }
  {
    // Single curves: zero tension is a line; positive = slow start; ±t mirror.
    bool lin = true, bend = true, mirror = true, mono = true;
    for (AutoShape sh : {AutoShape::Curve, AutoShape::Curve2, AutoShape::Curve3}) {
      lin = lin && near(autoShape(sh, 0, 0.3), 0.3, 1e-6);
      bend = bend && autoShape(sh, 0.5, 0.5) < 0.5 && autoShape(sh, -0.5, 0.5) > 0.5;
      for (double x = 0; x <= 1.0; x += 0.05) {
        mirror = mirror && near(1.0 - autoShape(sh, -0.6, 1.0 - x), autoShape(sh, 0.6, x), 1e-9);
        mono = mono && autoShape(sh, 0.8, x + 0.05) >= autoShape(sh, 0.8, x) - 1e-12;
      }
    }
    expect(lin, "single curves: zero tension is linear");
    expect(bend, "single curves: positive tension = slow start, negative = fast start");
    expect(mirror, "single curves: tension t and -t are mirror images");
    expect(mono, "single curves are monotonic");
    // Quarter-sine fit at |t| = 0.25 (FL's sine shape presets use these segments).
    double worst = 0;
    for (double x = 0; x <= 1.0; x += 0.01) worst = std::max(worst, std::fabs(autoShape(AutoShape::Curve, 0.25, x) - (1 - std::cos(kPi * x / 2))));
    expect(worst < 0.02, "single curve at tension 0.25 traces a quarter sine within 0.02");
  }
  {
    bool sym = true;
    for (AutoShape sh : {AutoShape::SCurve, AutoShape::SCurve2, AutoShape::SCurve3})
      for (double x = 0; x <= 1.0; x += 0.05) sym = sym && near(1.0 - autoShape(sh, 0.7, 1.0 - x), autoShape(sh, 0.7, x), 1e-9);
    expect(sym && near(autoShape(AutoShape::SCurve, 0.7, 0.5), 0.5, 1e-9), "double curves are point-symmetric about the middle");
    expect(autoShape(AutoShape::SCurve, 0.7, 0.25) < 0.25 && autoShape(AutoShape::SCurve, -0.7, 0.25) > 0.25, "double curve: S or inverse S by tension");
  }
  expect(near(autoShape(AutoShape::Hold, 0.5, 0.999), 0) && near(autoShape(AutoShape::Hold, 0.5, 1.0), 1), "hold jumps at the point");
  {
    auto distinct = [](AutoShape sh, double t) {
      std::vector<double> lv;
      for (int i = 0; i < 1000; ++i) {
        double v = autoShape(sh, t, i / 1000.0);
        if (lv.empty() || std::fabs(v - lv.back()) > 1e-6) lv.push_back(v);
      }
      return lv.size();
    };
    expect(distinct(AutoShape::Stairs, -1.0) == 2 && distinct(AutoShape::Stairs, 1.0) == 16, "stairs: tension sets 2 … 16 steps");
    size_t smooth = distinct(AutoShape::SmoothStairs, -1.0);
    expect(smooth > 20, "smooth stairs: risers are eased (many intermediate values)");
    double plateau = autoShape(AutoShape::SmoothStairs, -1.0, 0.1), plateau2 = autoShape(AutoShape::SmoothStairs, -1.0, 0.2);
    expect(near(plateau, plateau2, 1e-12), "smooth stairs: flat treads between risers");
  }
  {
    double m = autoShape(AutoShape::HalfSine, 0, 0.5);
    expect(near(m, 0.5, 1e-9) && near(autoShape(AutoShape::HalfSine, 1.0, 0.5), 1 - std::cos(kPi / 4), 1e-9) &&
               near(autoShape(AutoShape::HalfSine, -1.0, 0.5), std::sin(kPi / 4), 1e-9),
           "half sine: half cosine at 0, quarter-sine ease in / out at ±1");
    int crossings = 0;
    for (int i = 1; i < 1000; ++i)
      crossings += (autoShape(AutoShape::Pulse, 0, (i - 1) / 1000.0) < 0.5) != (autoShape(AutoShape::Pulse, 0, i / 1000.0) < 0.5);
    expect(crossings >= 14, "pulse: square wave between the two values");
  }
  {
    // FL mode bytes map onto all 13 modes, one to one.
    bool bij = true;
    for (int m = 0; m <= 12; ++m) bij = bij && ae::flModeOf(ae::shapeFromFlMode(m)) == m;
    expect(bij && ae::menuOrder().size() == static_cast<size_t>(AutoShape::Count), "FL curve modes 0-12 ↔ Gridline modes (13)");
    bool keys = true;
    for (int k = 0; k < static_cast<int>(AutoShape::Count); ++k)
      keys = keys && ae::shapeFromKey(ae::shapeKey(static_cast<AutoShape>(k))) == static_cast<AutoShape>(k);
    expect(keys, "shape keys round trip");
  }
  {
    AutomationClip c;
    c.points = {pt(0, 0), pt(96, 1)};
    c.loop = 192;
    expect(near(automationValue(c, 48), 0.5, 1e-6) && near(automationValue(c, 192 + 48), 0.5, 1e-6) && near(automationValue(c, 150), 1.0),
           "loop: content repeats every loop ticks");
  }

  // ----------------------------------------------------------- point edits ----
  {
    ae::Points p = {pt(0, 0.2f), pt(96, 0.8f), pt(192, 0.4f), pt(288, 0.6f)};
    ae::Points out;
    Tick dt = ae::movePoints(p, {1}, 500, 0.5, out);
    expect(dt == 96 && out[1].pos == 192 && near(out[1].value, 1.0), "move: limited by the next unselected point, value clamped");
    dt = ae::movePoints(p, {1, 2}, -500, 0, out);
    expect(dt == -96 && out[1].pos == 0 && out[2].pos == 96, "move: a selection moves as a block");
    ae::Points q = p;
    int added = ae::addPoint(q, pt(96, 0.5f));
    expect(added == 2 && q.size() == 5, "add: inserted after points on the same tick");
    expect(ae::deletePoints(q, {0, 1, 2, 3, 4}) == 4 && q.size() == 1, "delete: at least one point stays");
    q = p;
    ae::flipVertical(q, {});
    expect(near(q[0].value, 0.8, 1e-6) && near(q[3].value, 0.4, 1e-6), "flip vertically");
    q = {pt(0, 0.0f), pt(100, 1.0f, 0.5f, AutoShape::Curve), pt(300, 0.0f, 0, AutoShape::Hold)};
    ae::Points flipped = q;
    ae::flipHorizontal(flipped, {});
    expect(flipped[1].pos == 200 && near(ae::valueAt(flipped, 250), ae::valueAt(q, 50), 1e-6), "flip horizontally: time mirrored, curves reversed");
    q = p;
    ae::scaleLevels(q, {}, 0.5, 0.5, 0.1);
    expect(near(q[0].value, 0.45, 1e-6) && near(q[1].value, 0.75, 1e-6), "scale levels: centre, multiplier, offset");
    q = {pt(5, 0), pt(40, 1), pt(50, 0.5f), pt(100, 0.2f)};
    ae::Selection s = ae::quantize(q, {}, 24);
    expect(q.size() == 3 && q[0].pos == 0 && q[1].pos == 48 && near(q[1].value, 0.5) && q[2].pos == 96, "quantize: snaps, same tick keeps the last");
    s = ae::selectBox(p, 90, 200, 0.3, 0.9);
    expect(s == ae::Selection{1, 2}, "box select by time and value");
    ae::Points clip = ae::copyPoints(p, {1, 2});
    expect(clip.size() == 2 && clip[0].pos == 0 && clip[1].pos == 96 && ae::clipSpan(clip) == 96, "copy: positions relative to the first");
    q = p;
    s = ae::pastePoints(q, clip, 288, false);
    expect(q.size() == 6 && s.size() == 2 && q[*s.begin()].pos == 288 && q.back().pos == 384, "paste (insert) shifts later points");
    q = p;
    s = ae::pastePoints(q, clip, 0, true);
    expect(q.size() == 4 && q[0].pos == 0 && near(q[0].value, 0.8, 1e-6) && q[1].pos == 96 && near(q[1].value, 0.4, 1e-6), "paste (replace) overwrites its span");
    q = p;
    ae::setShape(q, {2}, AutoShape::Stairs);
    ae::setTension(q, {2}, 3.0f);
    expect(q[2].shape == AutoShape::Stairs && q[2].tension == 1.0f && q[1].shape == AutoShape::Curve, "per-point mode and tension");
  }
  {
    // Undo / redo through the document.
    Song s = makeEmptySong();
    AutomationClip c;
    c.id = s.allocateId();
    c.points = {pt(0, 0.1f), pt(96, 0.9f)};
    s.automations.push_back(c);
    Document doc(s);
    const ObjectId id = c.id;
    doc.editAutomation(id, "add point", [](AutomationClip& a) { ae::addPoint(a.points, pt(48, 0.5f)); });
    doc.editAutomation(id, "move", [](AutomationClip& a) { a.points[1].value = 0.7f; }, 77);
    doc.editAutomation(id, "move", [](AutomationClip& a) { a.points[1].value = 0.6f; }, 77);  // merges
    const AutomationClip* a = doc.song().automation(id);
    expect(a->points.size() == 3 && near(a->points[1].value, 0.6, 1e-6), "edits apply");
    doc.undo();
    a = doc.song().automation(id);
    expect(a->points.size() == 3 && near(a->points[1].value, 0.5, 1e-6), "undo: one step per merged drag");
    doc.undo();
    expect(doc.song().automation(id)->points.size() == 2, "undo: point add");
    doc.redo();
    doc.redo();
    expect(doc.song().automation(id)->points.size() == 3 && near(doc.song().automation(id)->points[1].value, 0.6, 1e-6), "redo");
    // Loop survives a save / load.
    doc.editAutomation(id, "loop", [](AutomationClip& x) { x.loop = 192; });
    Json json = songToJson(doc.song());
    Song back;
    std::string err;
    expect(songFromJson(json, back, &err) && back.automation(id) && back.automation(id)->loop == 192 &&
               back.automation(id)->points == doc.song().automation(id)->points,
           "loop and points round trip through the project file");
  }

  // ------------------------------------------------------------------- LFO ----
  {
    ae::LfoSettings l;
    l.shape = ae::LfoShape::Sine;
    l.periodStart = l.periodEnd = 96;
    l.centreStart = l.centreEnd = 0.5;
    l.amountStart = l.amountEnd = 0.4;
    ae::Points p = ae::lfoPoints(l, 0, 384);
    expect(p.size() == 17 && p.front().pos == 0 && p.back().pos == 384, "sine LFO: 4 points per cycle over 4 cycles + start");
    double worst = 0;
    for (int t = 0; t <= 384; ++t) worst = std::max(worst, std::fabs(ae::valueAt(p, t) - (0.5 + 0.4 * std::sin(2 * kPi * t / 96.0))));
    expect(worst < 0.012, "sine LFO follows a sine within 0.012");
    l.shape = ae::LfoShape::Square;
    l.pulseWidth = 0.25;
    p = ae::lfoPoints(l, 0, 192);
    expect(near(ae::valueAt(p, 10), 0.9, 1e-6) && near(ae::valueAt(p, 30), 0.1, 1e-6) && near(ae::valueAt(p, 100), 0.9, 1e-6),
           "square LFO: pulse width");
    l.shape = ae::LfoShape::SawUp;
    p = ae::lfoPoints(l, 0, 192);
    expect(near(ae::valueAt(p, 48), 0.5, 1e-3) && ae::valueAt(p, 95) > 0.85 && ae::valueAt(p, 97) < 0.15, "saw LFO ramps then drops");
    l.shape = ae::LfoShape::Triangle;
    l.periodStart = 192;
    l.periodEnd = 48;  // speeds up
    p = ae::lfoPoints(l, 0, 768);
    int peaks = 0;
    for (const auto& q : p) peaks += q.value > 0.85;
    expect(peaks >= 8, "speed sweep: more cycles as the period shrinks");
    l = ae::LfoSettings{};
    l.amountStart = 0.0;
    l.amountEnd = 0.5;
    p = ae::lfoPoints(l, 0, 960);
    expect(std::fabs(ae::valueAt(p, 24) - 0.5) < 0.03 && ae::valueAt(p, 960 - 72) > 0.95 - 0.06, "amount sweep: grows from 0 to full");
    l.phase = 0.25;
    l.amountEnd = 0.5;
    l.amountStart = 0.5;
    p = ae::lfoPoints(l, 0, 96);
    expect(near(p.front().value, 1.0, 1e-6), "phase offset");
    ae::Points base = {pt(0, 0.0f), pt(1000, 1.0f)};
    ae::applyLfo(base, ae::LfoSettings{}, 200, 392);
    expect(base.front().pos == 0 && base.back().pos == 1000 && base.size() == 2 + 9, "LFO replaces only its range");
  }

  // -------------------------------------------------------- recording thinning ----
  {
    ae::Points rec;
    for (int t = 0; t <= 960; t += 4) rec.push_back(pt(t, static_cast<float>(t < 480 ? t / 480.0 : 1.0 - (t - 480) / 960.0), 0, AutoShape::Hold));
    ae::Points th = ae::thin(rec, 0.005);
    expect(th.size() <= 4 && th.front().pos == 0 && th.back().pos == 960, "thinning: a ramp up/down keeps its corners only");
    rec.clear();
    for (int t = 0; t <= 960; t += 2) rec.push_back(pt(t, static_cast<float>(0.5 + 0.5 * std::sin(2 * kPi * t / 480.0)), 0, AutoShape::Hold));
    th = ae::thin(rec, 0.01);
    double worst = 0;
    for (const auto& r : rec) worst = std::max(worst, std::fabs(ae::valueAt(th, static_cast<double>(r.pos)) - r.value));
    expect(th.size() < rec.size() / 8 && worst <= 0.0101, "thinning: a sine keeps few points within the tolerance");
    ae::Points lane = {pt(0, 0.5f), pt(2000, 0.5f)};
    ae::mergeRecorded(lane, rec, 100, 900, 0.01);
    bool inRange = true;
    for (const auto& q : lane) inRange = inRange && (q.pos == 0 || q.pos == 2000 || (q.pos >= 100 && q.pos <= 900));
    expect(inRange && lane.front().pos == 0 && lane.back().pos == 2000 && lane.size() > 4, "recorded points replace only the recorded span");
  }

  // ---------------------------------------------------------------- shapes ----
  const fs::path root = fs::temp_directory_path() / "gridline-test-autoedit";
  std::error_code ec;
  fs::remove_all(root, ec);
  {
    AutomationShape sh;
    sh.points = {pt(0, 0.0f), pt(48, 1.0f, 0.3f, AutoShape::SCurve2), pt(96, 0.2f, -0.5f, AutoShape::HalfSine), pt(192, 0.6f, 0, AutoShape::Hold)};
    expect(saveAutomationShape(root.string(), sh, 96, "Mine/Rise"), "shape saves");
    auto list = listAutomationShapes(root.string());
    AutomationShape back;
    expect(list.size() == 1 && list[0].name == "Rise" && list[0].folder == "Mine" && loadAutomationShape(list[0].file, 96, back) &&
               back.points == sh.points,
           "shape round trip (folder, points, curves)");
    AutomationShape at960;
    expect(loadAutomationShape(list[0].file, 960, at960) && at960.points[3].pos == 1920, "shapes are stored in beats (any PPQ)");
    ae::Points tgt = {pt(0, 0.5f), pt(1000, 0.5f)};
    ae::applyShape(tgt, sh.points, 100, 484);
    expect(tgt.size() == 6 && tgt[1].pos == 100 && tgt[4].pos == 484 && tgt[2].shape == AutoShape::SCurve2, "shape fits into a range");
  }

  // ------------------------------------------------------- FL preset import ----
  {
    // FL 4-era (format 1, f32 records): a sine segment pair, an S-curve and a hold.
    std::vector<FlPoint> pts = {{0, 0.0, 0, 0}, {0.25, 0.5, -0.25f, 0}, {0.25, 1.0, 0.25f, 1}, {0.5, 0.0, 0, 2}};
    const fs::path fst = root / "fl" / "Data" / "Patches" / "Channel presets" / "Automation clips" / "Shapes" / "Wobble.fst";
    writeBytes(fst, automationFst(pts, 1));
    flp::PresetResult r;
    expect(flp::importFlPreset(fst.string(), r, "/nonexistent") && r.kind == flp::PresetResult::Shape && r.shape.points.size() == 4,
           "FL automation-clip preset (f32 records) → shape");
    const auto& sp = r.shape.points;
    expect(sp[1].pos == 240 && sp[2].pos == 480 && sp[3].pos == 960 && near(sp[2].value, 1.0, 1e-6) && sp[2].shape == AutoShape::SCurve &&
               sp[3].shape == AutoShape::Hold,
           "FL points: beat deltas, values, modes");
    expect(near(sp[1].tension, 0.25, 1e-6) && near(sp[2].tension, -0.25, 1e-6), "FL tension sign converted (FL negative = slow start)");
    // Newer layout (format 2+, f64 records) and the library walk.
    const fs::path fst2 = root / "fl" / "Data" / "Patches" / "Channel presets" / "Automation clips" / "Special" / "Ramp.fst";
    writeBytes(fst2, automationFst({{0, 0.25, 0, 0}, {4, 0.75, 0.5f, 7}}, 3));
    flp::PresetResult r2;
    expect(flp::importFlPreset(fst2.string(), r2, "/nonexistent") && r2.shape.points.size() == 2 && r2.shape.points[1].pos == 4 * flp::kShapePpq &&
               r2.shape.points[1].shape == AutoShape::Curve2 && near(r2.shape.points[1].tension, -0.5, 1e-6),
           "FL automation preset (f64 records)");
    const fs::path lib = root / "lib";
    auto rep = flp::importPresetLibrary((root / "fl").string(), lib.string());
    auto shapes = listAutomationShapes(lib.string());
    AutomationShape loaded;
    expect(rep.shapes == 2 && shapes.size() == 2 && shapes[0].folder == "FL Studio/Shapes" && shapes[1].folder == "FL Studio/Special" &&
               loadAutomationShape(shapes[0].file, 96, loaded) && loaded.points.size() == 4 && loaded.points[3].pos == 96,
           "import-fl-presets: automation-clip presets land in the shapes library");
  }
  fs::remove_all(root, ec);
  return finish();
}
